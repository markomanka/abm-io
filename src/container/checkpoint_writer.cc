// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <set>
#include <vector>

#include "abmio/checkpoint.h"
#include "container/h5store.h"
#include "durable.h"

namespace abmio {
namespace {

Status Fail(Status s, std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message;
  }
  return s;
}

// The group of a variable-size array: its elements, and offsets that start
// with 0 so that an empty table already has its one entry.
Status CreateVector(hid_t parent, const std::string& name, ElementType element,
                    const char* values_name, const h5::ColumnLayout& layout,
                    h5::Column* values, h5::Column* offsets, std::string* why) {
  h5::Id group;
  Status s = h5::Group(parent, name, true, &group, why);
  if (s == Status::kOk) {
    s = h5::Column::Create(group.get(), values_name, element, 1, layout, values,
                           why);
  }
  if (s == Status::kOk) {
    s = h5::Column::Create(group.get(), "offsets", ElementType::kUInt64, 1,
                           layout, offsets, why);
  }
  if (s == Status::kOk) {
    const uint64_t zero = 0;
    s = offsets->Append(&zero, 1, why);
  }
  return s;
}

// The group a column at `path` ("inner/x") goes into, created on the way,
// and the column's own name.
Status ParentOf(hid_t group, const std::string& path, h5::Id* holder,
                hid_t* parent, std::string* leaf, std::string* why) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    *parent = group;
    *leaf = path;
    return Status::kOk;
  }
  Status s = h5::Group(group, path.substr(0, slash), true, holder, why);
  *parent = holder->get();
  *leaf = path.substr(slash + 1);
  return s;
}

}  // namespace

namespace detail {

std::string VersionsOf(const TableSpec& spec) {
  std::string s;
  for (const TypeDescription& t : spec.types) {
    s += (s.empty() ? "" : ";") + t.qualified_name + "=" +
         std::to_string(t.version);
  }
  return s;
}

std::string UnitsOf(const TableSpec& spec) {
  std::string s;
  for (const FieldDescription& f : spec.columns) {
    if (!f.unit.empty()) {
      s += (s.empty() ? "" : ";") + f.name + "=" + f.unit;
    }
  }
  return s;
}

}  // namespace detail

struct CheckpointWriter::Table {
  std::string path;
  std::string type;
  std::vector<FieldDescription> fields;
  // Used per field by its kind; the others stay empty. A variable-size array
  // keeps its elements in `columns` and its offsets in `offsets`, and
  // `totals` holds its last offset so far.
  std::vector<h5::Column> columns;
  std::vector<h5::StringColumn> strings;
  std::vector<h5::Column> offsets;
  std::vector<uint64_t> totals;
  uint64_t rows = 0;
  // A list of behaviours keeps its type identifiers in `columns` and its
  // offsets in `offsets`, and the tables of its subtypes here.
  std::vector<std::vector<std::unique_ptr<Table>>> children;
};

struct CheckpointWriter::Impl {
  h5::Id file;
  std::string path;       // the final name, given at Close
  std::string temporary;  // the name written to until then
  CheckpointOptions options;
  Schema schema;
  std::set<TypeId> described;
  std::set<std::string> paths;
  std::vector<std::unique_ptr<Table>> tables;
};

CheckpointWriter::CheckpointWriter() : impl_(new Impl) {}
CheckpointWriter::~CheckpointWriter() = default;

Status CheckpointWriter::Open(const std::string& path,
                              const CheckpointOptions& options,
                              std::string* why) {
  if (options.chunk_rows == 0) {
    return Fail(Status::kBadSchema, why,
                "a checkpoint needs at least one row per chunk");
  }
  impl_.reset(new Impl);
  impl_->options = options;
  impl_->path = path;
  impl_->temporary = path + ".tmp";
  Status s = h5::CreateFile(impl_->temporary, &impl_->file, why);
  if (s != Status::kOk) {
    return s;
  }
  s = h5::WriteAttribute(impl_->file.get(), "format", kCheckpointFormatName,
                         why);
  if (s != Status::kOk) {
    return s;
  }
  return h5::WriteAttribute(impl_->file.get(), "format_version",
                            std::to_string(kCheckpointFormat), why);
}

uint64_t CheckpointWriter::chunk_rows() const {
  return impl_->options.chunk_rows;
}

Status CheckpointWriter::Refuse(const std::string& path,
                                const std::string& problem, std::string* why) {
  return Fail(Status::kBadSchema, why, "cannot write " + path + ": " + problem);
}

Status CheckpointWriter::BeginTable(const std::string& path,
                                    const detail::TableSpec& spec,
                                    Table** table, std::string* why) {
  if (!impl_->file.ok()) {
    return Fail(Status::kIo, why, "the checkpoint is not open");
  }
  std::unique_ptr<Table> t;
  Status s = CreateTable(path, spec, &t, why);
  if (s != Status::kOk) {
    return s;
  }
  *table = t.get();
  impl_->tables.push_back(std::move(t));
  return Status::kOk;
}

Status CheckpointWriter::CreateTable(const std::string& path,
                                     const detail::TableSpec& spec,
                                     std::unique_ptr<Table>* table,
                                     std::string* why) {
  if (path.empty() || !impl_->paths.insert(path).second) {
    return Fail(Status::kBadSchema, why,
                "the table " + path + " was already written");
  }
  auto t = std::make_unique<Table>();
  t->path = path;
  t->type = spec.type;

  // Every field with the checkpoint role, base types first and nested
  // records in place: the order in which the visitor will hand them over.
  std::set<std::string> names;
  for (const FieldDescription& f : spec.columns) {
    const std::string where = spec.type + "." + f.name;
    if (!names.insert(f.name).second) {
      return Fail(Status::kBadSchema, why,
                  where + " repeats the name of another field");
    }
    // StoredAs is ignored: checkpoints keep full precision, so that a
    // restore is exact (docs/RATIONALE.md, entry 14).
    if (f.kind != Kind::kScalar && f.kind != Kind::kArray &&
        f.kind != Kind::kVector && f.kind != Kind::kString &&
        f.kind != Kind::kPolymorphicList) {
      return Fail(Status::kBadSchema, why,
                  where + " is a " + ToString(f.kind) +
                      ", which checkpoints do not write");
    }
    t->fields.push_back(f);
  }

  h5::Id group;
  Status s = h5::Group(impl_->file.get(), path, true, &group, why);
  if (s != Status::kOk) {
    return s;
  }
  s = h5::WriteAttribute(group.get(), "abmio_type", t->type, why);
  if (s == Status::kOk) {
    s = h5::WriteAttribute(group.get(), "abmio_versions",
                           detail::VersionsOf(spec), why);
  }
  if (s == Status::kOk) {
    s = h5::WriteAttribute(group.get(), "abmio_units", detail::UnitsOf(spec),
                           why);
  }
  if (s != Status::kOk) {
    return s;
  }
  h5::ColumnLayout layout;
  layout.chunk_rows = impl_->options.chunk_rows;
  layout.lz4 = impl_->options.lz4;
  const std::size_t n = t->fields.size();
  t->columns.resize(n);
  t->strings.resize(n);
  t->offsets.resize(n);
  t->totals.assign(n, 0);
  t->children.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const FieldDescription& f = t->fields[i];
    if (f.kind == Kind::kVector) {
      s = CreateVector(group.get(), f.name, f.element, "values", layout,
                       &t->columns[i], &t->offsets[i], why);
    } else if (f.kind == Kind::kPolymorphicList) {
      // The list's type identifiers and offsets, and beside them one table
      // per listed subtype, named after it.
      s = CreateVector(group.get(), f.name, ElementType::kUInt64, "types",
                       layout, &t->columns[i], &t->offsets[i], why);
      for (const detail::TableSpec& child : spec.children[i]) {
        if (s != Status::kOk) {
          break;
        }
        t->children[i].emplace_back();
        s = CreateTable(path + "/" + f.name + "/" + child.type, child,
                        &t->children[i].back(), why);
      }
    } else {
      h5::Id holder;
      hid_t parent = -1;
      std::string leaf;
      s = ParentOf(group.get(), f.name, &holder, &parent, &leaf, why);
      if (s == Status::kOk) {
        s = f.kind == Kind::kString
                ? h5::StringColumn::Create(parent, leaf, layout.chunk_rows,
                                           &t->strings[i], why)
                : h5::Column::Create(parent, leaf, f.element, f.count, layout,
                                     &t->columns[i], why);
      }
    }
    if (s != Status::kOk) {
      return s;
    }
  }
  for (const TypeDescription& type : spec.types) {
    if (impl_->described.insert(type.id).second) {
      impl_->schema.types.push_back(type);
    }
  }
  *table = std::move(t);
  return Status::kOk;
}

Status CheckpointWriter::AppendBlock(Table* table,
                                     std::vector<detail::ColumnBuffer>& block,
                                     uint64_t rows, bool overflowed,
                                     std::string* why) {
  const std::string mismatch =
      "the fields " + table->type + " hands over do not match its description";
  if (overflowed || block.size() != table->fields.size()) {
    return Fail(Status::kBadSchema, why, mismatch);
  }
  for (std::size_t i = 0; i < table->fields.size(); ++i) {
    const FieldDescription& f = table->fields[i];
    Status s = Status::kOk;
    if (f.kind == Kind::kString) {
      if (block[i].strings.size() != rows) {
        return Fail(Status::kBadSchema, why, mismatch + ": " + f.name);
      }
      s = table->strings[i].Append(block[i].strings, why);
    } else if (f.kind == Kind::kVector || f.kind == Kind::kPolymorphicList) {
      if (block[i].lengths.size() != rows) {
        return Fail(Status::kBadSchema, why, mismatch + ": " + f.name);
      }
      std::vector<uint64_t> ends(rows);
      uint64_t total = table->totals[i];
      uint64_t elements = 0;
      for (uint64_t r = 0; r < rows; ++r) {
        const uint64_t length = block[i].lengths[r];
        if (length > UINT64_MAX - total) {
          return Fail(Status::kBadSchema, why,
                      f.name + " has more elements than its offsets can count");
        }
        total += length;
        elements += length;
        ends[r] = total;
      }
      const std::size_t size = f.kind == Kind::kPolymorphicList
                                   ? sizeof(uint64_t)
                                   : SizeOf(f.element);
      if (block[i].bytes.size() / size != elements ||
          block[i].bytes.size() % size != 0) {
        return Fail(Status::kBadSchema, why, mismatch + ": " + f.name);
      }
      s = table->columns[i].Append(block[i].bytes.data(), elements, why);
      if (s == Status::kOk) {
        s = table->offsets[i].Append(ends.data(), rows, why);
      }
      if (s == Status::kOk) {
        table->totals[i] = total;
      }
      for (std::size_t k = 0;
           s == Status::kOk && f.kind == Kind::kPolymorphicList &&
           k < table->children[i].size();
           ++k) {
        if (k >= block[i].children.size()) {
          return Fail(Status::kBadSchema, why, mismatch + ": " + f.name);
        }
        if (block[i].child_rows[k] > 0) {
          s = AppendBlock(table->children[i][k].get(), block[i].children[k],
                          block[i].child_rows[k], false, why);
        }
      }
    } else {
      if (block[i].bytes.size() != rows * f.count * SizeOf(f.element)) {
        return Fail(Status::kBadSchema, why, mismatch + ": " + f.name);
      }
      s = table->columns[i].Append(block[i].bytes.data(), rows, why);
    }
    if (s != Status::kOk) {
      return s;
    }
  }
  table->rows += rows;
  return Status::kOk;
}

Status CheckpointWriter::Close(std::string* why) {
  if (!impl_->file.ok()) {
    return Fail(Status::kIo, why, "the checkpoint is not open");
  }
  // The columns close before the file does.
  impl_->tables.clear();
  h5::StringColumn schema;
  Status s =
      h5::StringColumn::Create(impl_->file.get(), "schema", 1, &schema, why);
  if (s == Status::kOk) {
    s = schema.Append({ToJson(impl_->schema)}, why);
  }
  schema = h5::StringColumn();
  impl_->file.Reset();
  if (s != Status::kOk) {
    return s;
  }
  // The file takes its final name only now, whole.
  return durable::Replace(impl_->temporary, impl_->path, why);
}

}  // namespace abmio
