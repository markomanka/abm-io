// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "abmio/checkpoint.h"
#include "container/h5store.h"

namespace abmio {
namespace {

Status Fail(Status s, std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message;
  }
  return s;
}

std::string Shape(ElementType element, uint32_t count) {
  std::string s = ToString(element);
  if (count != 1) {
    s += "[" + std::to_string(count) + "]";
  }
  return s;
}

}  // namespace

struct CheckpointReader::Table {
  std::string path;
  std::vector<FieldDescription> fields;
  // Used per field by its kind, as in the writer: a variable-size field keeps
  // its elements in `columns` and its offsets in `offsets`.
  std::vector<h5::Column> columns;
  std::vector<h5::StringColumn> strings;
  std::vector<h5::Column> offsets;
  uint64_t rows = 0;
};

struct CheckpointReader::Impl {
  h5::Id file;
  CheckpointOptions options;
  std::unique_ptr<Table> table;
};

CheckpointReader::CheckpointReader() : impl_(new Impl) {}
CheckpointReader::~CheckpointReader() = default;

Status CheckpointReader::Open(const std::string& path,
                              const CheckpointOptions& options,
                              std::string* why) {
  if (options.chunk_rows == 0) {
    return Fail(Status::kBadSchema, why, "chunk_rows must be at least 1");
  }
  impl_->options = options;
  return h5::OpenFile(path, &impl_->file, why);
}

uint64_t CheckpointReader::RowsOf(const Table* table) const {
  return table->rows;
}

std::size_t CheckpointReader::ColumnsOf(const Table* table) const {
  return table->fields.size();
}

uint64_t CheckpointReader::chunk_rows() const {
  return impl_->options.chunk_rows;
}

void CheckpointReader::EndTable() { impl_->table.reset(); }

Status CheckpointReader::BeginTable(const std::string& path,
                                    const std::vector<TypeDescription>& chain,
                                    Table** table, std::string* why) {
  std::string scratch;
  if (why == nullptr) {
    why = &scratch;
  }
  if (!impl_->file.ok()) {
    return Fail(Status::kIo, why, "the checkpoint is not open");
  }
  const std::string& type = chain.back().qualified_name;
  const std::string why_not = "cannot read " + path + " as " + type + ": ";
  auto t = std::make_unique<Table>();
  t->path = path;
  for (const TypeDescription& d : chain) {
    for (const FieldDescription& f : d.fields) {
      if (!HasRole(f.roles, Role::kCheckpoint)) {
        continue;
      }
      if (f.stored != ElementType::kNone && f.stored != f.element) {
        return Fail(Status::kBadSchema, why,
                    why_not + f.name +
                        " is declared StoredAs a narrower type, which "
                        "checkpoints do not read yet");
      }
      if (f.kind != Kind::kScalar && f.kind != Kind::kArray &&
          f.kind != Kind::kVector && f.kind != Kind::kString) {
        return Fail(Status::kBadSchema, why,
                    why_not + f.name + " is a " + ToString(f.kind) +
                        ", which checkpoints do not read yet");
      }
      t->fields.push_back(f);
    }
  }

  h5::Id group;
  Status s = h5::Group(impl_->file.get(), path, false, &group, why);
  if (s != Status::kOk) {
    return Fail(s, why, why_not + "the file has no table there");
  }
  std::string stored_type;
  s = h5::ReadAttribute(group.get(), "abmio_type", &stored_type, why);
  if (s != Status::kOk) {
    return Fail(Status::kCorrupt, why, why_not + "it does not name its type");
  }
  if (stored_type != type) {
    return Fail(Status::kBadSchema, why,
                why_not + "the file holds " + stored_type + " there");
  }

  // Every column in the file must be a field of the model, and every field a
  // column: resuming takes no defaults and drops nothing.
  std::vector<std::string> members;
  s = h5::Members(group.get(), &members, why);
  if (s != Status::kOk) {
    return s;
  }
  std::set<std::string> expected;
  for (const FieldDescription& f : t->fields) {
    expected.insert(f.name);
  }
  for (const std::string& m : members) {
    if (expected.count(m) == 0) {
      return Fail(
          Status::kBadSchema, why,
          why_not + "the file has a field " + m + " that the model does not");
    }
  }

  const std::size_t n = t->fields.size();
  t->columns.resize(n);
  t->strings.resize(n);
  t->offsets.resize(n);
  bool first = true;
  for (std::size_t i = 0; i < n; ++i) {
    const FieldDescription& f = t->fields[i];
    const std::string field = why_not + "field " + f.name;
    const h5::Member member = h5::MemberOf(group.get(), f.name);
    if (member == h5::Member::kNone) {
      return Fail(Status::kBadSchema, why,
                  field + " is in the model but not in the file");
    }
    uint64_t rows = 0;
    if (f.kind == Kind::kVector) {
      if (member != h5::Member::kGroup) {
        return Fail(Status::kBadSchema, why,
                    field + " is a vector in the model but not in the file");
      }
      h5::Id vector;
      s = h5::Group(group.get(), f.name, false, &vector, why);
      if (s == Status::kOk) {
        s = h5::Column::Open(vector.get(), "values", &t->columns[i], why);
      }
      if (s == Status::kOk) {
        s = h5::Column::Open(vector.get(), "offsets", &t->offsets[i], why);
      }
      if (s != Status::kOk) {
        return Fail(Status::kCorrupt, why, field + ": " + *why);
      }
      const h5::Column& values = t->columns[i];
      const h5::Column& offsets = t->offsets[i];
      if (values.element() != f.element || values.count() != 1) {
        return Fail(
            Status::kBadSchema, why,
            field + " holds " + Shape(values.element(), values.count()) +
                " in the file and " + ToString(f.element) + " in the model");
      }
      if (offsets.element() != ElementType::kUInt64 || offsets.count() != 1 ||
          offsets.rows() == 0) {
        return Fail(Status::kCorrupt, why, field + " has malformed offsets");
      }
      uint64_t ends[2] = {0, 0};
      s = offsets.Read(0, 1, &ends[0], why);
      if (s == Status::kOk) {
        s = offsets.Read(offsets.rows() - 1, 1, &ends[1], why);
      }
      if (s != Status::kOk) {
        return Fail(s, why, field + ": " + *why);
      }
      if (ends[0] != 0 || ends[1] != values.rows()) {
        return Fail(Status::kCorrupt, why,
                    field +
                        " has offsets that do not start at 0 and end at "
                        "the number of values");
      }
      rows = offsets.rows() - 1;
    } else if (f.kind == Kind::kString) {
      s = member == h5::Member::kDataset
              ? h5::StringColumn::Open(group.get(), f.name, &t->strings[i], why)
              : Status::kBadSchema;
      if (s != Status::kOk) {
        return Fail(Status::kBadSchema, why,
                    field + " is a string in the model but not in the file");
      }
      rows = t->strings[i].rows();
    } else {
      s = member == h5::Member::kDataset
              ? h5::Column::Open(group.get(), f.name, &t->columns[i], why)
              : Status::kBadSchema;
      if (s != Status::kOk) {
        return Fail(Status::kBadSchema, why,
                    field + " is not a column of numbers in the file");
      }
      const h5::Column& c = t->columns[i];
      if (c.element() != f.element || c.count() != f.count) {
        return Fail(Status::kBadSchema, why,
                    field + " holds " + Shape(c.element(), c.count()) +
                        " in the file and " + Shape(f.element, f.count) +
                        " in the model");
      }
      rows = c.rows();
    }
    if (first) {
      t->rows = rows;
      first = false;
    } else if (rows != t->rows) {
      return Fail(Status::kCorrupt, why,
                  field + " has " + std::to_string(rows) +
                      " rows where the fields before it have " +
                      std::to_string(t->rows));
    }
  }
  *table = t.get();
  impl_->table = std::move(t);
  return Status::kOk;
}

Status CheckpointReader::ReadBlock(Table* table, uint64_t first, uint64_t rows,
                                   std::vector<detail::ColumnBuffer>& block,
                                   std::string* why) {
  for (std::size_t i = 0; i < table->fields.size(); ++i) {
    const FieldDescription& f = table->fields[i];
    detail::ColumnBuffer& column = block[i];
    column.bytes.clear();
    column.strings.clear();
    column.lengths.clear();
    Status s = Status::kOk;
    if (f.kind == Kind::kString) {
      s = table->strings[i].Read(first, rows, &column.strings, why);
    } else if (f.kind == Kind::kVector) {
      std::vector<uint64_t> ends(rows + 1);
      s = table->offsets[i].Read(first, rows + 1, ends.data(), why);
      if (s != Status::kOk) {
        return s;
      }
      const uint64_t values = table->columns[i].rows();
      for (uint64_t r = 0; r < rows; ++r) {
        if (ends[r + 1] < ends[r] || ends[r + 1] > values) {
          return Fail(Status::kCorrupt, why,
                      "cannot read " + table->path + ": field " + f.name +
                          " has offsets that decrease or pass the end of its "
                          "values, at row " +
                          std::to_string(first + r));
        }
        column.lengths.push_back(ends[r + 1] - ends[r]);
      }
      const uint64_t elements = ends[rows] - ends[0];
      column.bytes.resize(elements * SizeOf(f.element));
      s = table->columns[i].Read(ends[0], elements, column.bytes.data(), why);
    } else {
      column.bytes.resize(rows * f.count * SizeOf(f.element));
      s = table->columns[i].Read(first, rows, column.bytes.data(), why);
    }
    if (s != Status::kOk) {
      return s;
    }
  }
  return Status::kOk;
}

Status CheckpointReader::CountDiffers(const std::string& path, uint64_t count,
                                      uint64_t rows, std::string* why) {
  return Fail(Status::kSizeMismatch, why,
              path + " holds " + std::to_string(rows) + " objects, not " +
                  std::to_string(count));
}

Status CheckpointReader::BlockMismatch(const std::string& path,
                                       std::string* why) {
  return Fail(Status::kCorrupt, why,
              "cannot read " + path +
                  ": a block does not match the fields it was checked against");
}

}  // namespace abmio
