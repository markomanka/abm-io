// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
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

// "a=1;b=2" as pairs, in order.
std::vector<std::pair<std::string, std::string>> Pairs(const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out;
  std::size_t start = 0;
  while (start < s.size()) {
    std::size_t end = s.find(';', start);
    if (end == std::string::npos) {
      end = s.size();
    }
    const std::string item = s.substr(start, end - start);
    const std::size_t eq = item.find('=');
    out.emplace_back(item.substr(0, eq),
                     eq == std::string::npos ? "" : item.substr(eq + 1));
    start = end + 1;
  }
  return out;
}

// The first difference between what the file records and what the model
// says, as "x is A in the file and B in the model", or "" if none.
std::string FirstDifference(const std::string& file, const std::string& model,
                            const char* what) {
  if (file == model) {
    return "";
  }
  const auto in_file = Pairs(file);
  const auto in_model = Pairs(model);
  auto find = [](const std::vector<std::pair<std::string, std::string>>& v,
                 const std::string& key) -> const std::string* {
    for (const auto& p : v) {
      if (p.first == key) {
        return &p.second;
      }
    }
    return nullptr;
  };
  for (const auto& m : in_model) {
    const std::string* f = find(in_file, m.first);
    if (f == nullptr) {
      return m.first + " has " + what + " " + m.second +
             " in the model and none in the file";
    }
    if (*f != m.second) {
      return m.first + " has " + what + " " + *f + " in the file and " +
             m.second + " in the model";
    }
  }
  for (const auto& f : in_file) {
    if (find(in_model, f.first) == nullptr) {
      return f.first + " has " + what + " " + f.second +
             " in the file and none in the model";
    }
  }
  return std::string("the ") + what + "s are in another order: " + file +
         " in the file, " + model + " in the model";
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
  Status s = h5::OpenFile(path, &impl_->file, why);
  if (s != Status::kOk) {
    return s;
  }
  // The file says what it is and in which version of the format, and a
  // version this abm-io does not know is refused rather than guessed at.
  std::string format, version;
  if (h5::ReadAttribute(impl_->file.get(), "format", &format, why) !=
          Status::kOk ||
      format != kCheckpointFormatName ||
      h5::ReadAttribute(impl_->file.get(), "format_version", &version, why) !=
          Status::kOk) {
    impl_->file.Reset();
    return Fail(Status::kBadHeader, why, path + " is not an abm-io checkpoint");
  }
  if (version != std::to_string(kCheckpointFormat)) {
    impl_->file.Reset();
    return Fail(Status::kBadHeader, why,
                path + " is a checkpoint of format version " + version +
                    "; this abm-io reads version " +
                    std::to_string(kCheckpointFormat));
  }
  return Status::kOk;
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
  // The types' versions and the fields' units: the same columns under another
  // version, or a length in other units, are not the same state.
  std::string versions, units;
  if (h5::ReadAttribute(group.get(), "abmio_versions", &versions, why) !=
          Status::kOk ||
      h5::ReadAttribute(group.get(), "abmio_units", &units, why) !=
          Status::kOk) {
    return Fail(Status::kCorrupt, why,
                why_not + "it does not record its types' versions and units");
  }
  std::string differs =
      FirstDifference(versions, detail::VersionsOf(chain), "version");
  if (differs.empty()) {
    differs = FirstDifference(units, detail::UnitsOf(chain), "unit");
  }
  if (!differs.empty()) {
    return Fail(Status::kBadSchema, why, why_not + differs);
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

Status CheckpointReader::CheckResume(const std::vector<ComponentRecord>& now,
                                     const ResumeAcceptance& acceptance,
                                     std::vector<LineageRecord>* lineage,
                                     std::string* why) {
  std::string scratch;
  if (why == nullptr) {
    why = &scratch;
  }
  if (!acceptance.components.empty() &&
      (acceptance.accepted_by.empty() || acceptance.reason.empty())) {
    return Fail(Status::kBadSchema, why,
                "accepting a changed component needs who accepts it and why");
  }
  SimulationRecord sim;
  Status s = ReadRecord(kSimulationRecord, &sim, why);
  std::vector<ComponentRecord> before;
  std::vector<LineageRecord> carried;
  uint64_t rows = 0;
  if (s == Status::kOk) {
    s = Rows<ComponentRecord>(kComponentsTable, &rows, why);
  }
  if (s == Status::kOk) {
    before.resize(rows);
    s = ReadTable<ComponentRecord>(
        kComponentsTable, rows,
        [&before](uint64_t i) -> ComponentRecord& { return before[i]; }, why);
  }
  if (s == Status::kOk) {
    s = Rows<LineageRecord>(kLineageTable, &rows, why);
  }
  if (s == Status::kOk) {
    carried.resize(rows);
    s = ReadTable<LineageRecord>(
        kLineageTable, rows,
        [&carried](uint64_t i) -> LineageRecord& { return carried[i]; }, why);
  }
  if (s != Status::kOk) {
    return Fail(s, why, "cannot resume from this checkpoint: " + *why);
  }

  std::vector<std::string> blocking;
  if (BlockingDifferences(before, now, acceptance.components, &blocking)) {
    std::string message =
        "cannot resume: components that affect results have changed, and "
        "none of these was accepted by name:";
    for (const std::string& b : blocking) {
      message += "\n  " + b;
    }
    return Fail(Status::kBadSchema, why, message);
  }

  auto version_of = [](const std::vector<ComponentRecord>& v,
                       const std::string& name) {
    for (const ComponentRecord& c : v) {
      if (c.name == name) {
        return c.version;
      }
    }
    return std::string();
  };
  for (const std::string& name : DifferingComponents(before, now)) {
    LineageRecord row;
    row.step = sim.step;
    row.component = name;
    row.from_version = version_of(before, name);
    row.to_version = version_of(now, name);
    row.accepted_by = acceptance.accepted_by;
    row.reason = acceptance.reason;
    row.at = acceptance.at;
    carried.push_back(row);
  }
  *lineage = std::move(carried);
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
