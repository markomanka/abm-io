// SPDX-License-Identifier: Apache-2.0
#include <set>

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

}  // namespace

struct CheckpointWriter::Table {
  std::string path;
  std::string type;
  std::vector<FieldDescription> fields;
  // One of the two is used per field, by its kind; the other stays empty.
  std::vector<h5::Column> columns;
  std::vector<h5::StringColumn> strings;
  uint64_t rows = 0;
};

struct CheckpointWriter::Impl {
  h5::Id file;
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
  Status s = h5::CreateFile(path, &impl_->file, why);
  if (s != Status::kOk) {
    return s;
  }
  s = h5::WriteAttribute(impl_->file.get(), "format", "abm-io checkpoint", why);
  if (s != Status::kOk) {
    return s;
  }
  return h5::WriteAttribute(impl_->file.get(), "format_version", "1", why);
}

uint64_t CheckpointWriter::chunk_rows() const {
  return impl_->options.chunk_rows;
}

std::size_t CheckpointWriter::ColumnsOf(const Table* table) const {
  return table->fields.size();
}

Status CheckpointWriter::BeginTable(const std::string& path,
                                    const std::vector<TypeDescription>& chain,
                                    Table** table, std::string* why) {
  if (!impl_->file.ok()) {
    return Fail(Status::kIo, why, "the checkpoint is not open");
  }
  if (path.empty() || !impl_->paths.insert(path).second) {
    return Fail(Status::kBadSchema, why,
                "the table " + path + " was already written");
  }
  auto t = std::make_unique<Table>();
  t->path = path;
  t->type = chain.back().qualified_name;

  // Every field with the checkpoint role, base types first: the order in
  // which the visitor will hand them over.
  std::set<std::string> names;
  for (const TypeDescription& type : chain) {
    for (const FieldDescription& f : type.fields) {
      if (!HasRole(f.roles, Role::kCheckpoint)) {
        continue;
      }
      const std::string where = type.qualified_name + "." + f.name;
      if (!names.insert(f.name).second) {
        return Fail(Status::kBadSchema, why,
                    where + " repeats the name of another field");
      }
      if (f.stored != ElementType::kNone && f.stored != f.element) {
        return Fail(Status::kBadSchema, why,
                    where +
                        " is declared StoredAs a narrower type, which "
                        "checkpoints do not "
                        "write yet");
      }
      if (f.kind != Kind::kScalar && f.kind != Kind::kArray &&
          f.kind != Kind::kString) {
        return Fail(Status::kBadSchema, why,
                    where + " is a " + ToString(f.kind) +
                        ", which checkpoints do not write yet");
      }
      t->fields.push_back(f);
    }
  }

  h5::Id group;
  Status s = h5::Group(impl_->file.get(), path, true, &group, why);
  if (s != Status::kOk) {
    return s;
  }
  s = h5::WriteAttribute(group.get(), "abmio_type", t->type, why);
  if (s != Status::kOk) {
    return s;
  }
  h5::ColumnLayout layout;
  layout.chunk_rows = impl_->options.chunk_rows;
  layout.lz4 = impl_->options.lz4;
  t->columns.resize(t->fields.size());
  t->strings.resize(t->fields.size());
  for (std::size_t i = 0; i < t->fields.size(); ++i) {
    const FieldDescription& f = t->fields[i];
    s = f.kind == Kind::kString
            ? h5::StringColumn::Create(group.get(), f.name, layout.chunk_rows,
                                       &t->strings[i], why)
            : h5::Column::Create(group.get(), f.name, f.element, f.count,
                                 layout, &t->columns[i], why);
    if (s != Status::kOk) {
      return s;
    }
  }
  for (const TypeDescription& type : chain) {
    if (impl_->described.insert(type.id).second) {
      impl_->schema.types.push_back(type);
    }
  }
  *table = t.get();
  impl_->tables.push_back(std::move(t));
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
  return s;
}

}  // namespace abmio
