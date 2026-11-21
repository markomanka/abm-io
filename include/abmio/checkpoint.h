// SPDX-License-Identifier: Apache-2.0
// Writing and reading a checkpoint (docs/INTEGRATION.md, "Checkpoint files").
//
// A checkpoint holds tables, many objects of a described type, and records,
// exactly one; a record is a table with one row. Objects are walked through
// the visitor protocol a block at a time, so the writer's memory is bounded by
// fields × chunk and does not grow with the number of objects.
//
// A variable-size array becomes two columns in a group named after the field,
// as in Apache Arrow: `values`, every object's elements one after another, and
// `offsets`, one more entry than there are objects, starting at 0, so that
// object i owns values[offsets[i], offsets[i + 1]) (docs/RATIONALE.md, entry
// 13). Such a field bounds a block's memory by its total number of elements,
// not by the chunk alone.
//
// A nested record's fields take columns of their own, in a group named after
// the record ("phase/clock/period"). A list of behaviours is a group named
// after the field holding its type identifiers and offsets, like a vector's,
// and one table per subtype listed by the base, named after it, holding that
// subtype's objects in the order they appear in the lists. A field declared
// StoredAs a narrower type is stored at full precision all the same:
// checkpoints restore exactly (docs/RATIONALE.md, entry 14).
//
// The file is written as <path>.tmp and renamed to <path> in Close, after
// being flushed to disk, so it never appears half-written; a store of
// checkpoints with a manifest and a pointer is in abmio/store.h.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "abmio/records.h"
#include "abmio/schema.h"
#include "abmio/status.h"
#include "abmio/visit.h"

namespace abmio {

struct CheckpointOptions {
  uint64_t chunk_rows =
      65536;        ///< objects per block, in memory and in the file
  bool lz4 = true;  ///< compress numeric columns with LZ4
};

namespace detail {

/// One field's values for one block of objects.
struct ColumnBuffer {
  std::vector<char> bytes;  ///< fixed-size fields, element after element;
                            ///< variable-size arrays, every element in turn
  std::vector<std::string> strings;  ///< string fields
  std::vector<uint64_t> lengths;     ///< variable-size arrays and lists, one
                                     ///< per object
  /// Lists of behaviours: per listed subtype, the block of its own table and
  /// how many of its objects the block holds.
  std::vector<std::vector<ColumnBuffer>> children;
  std::vector<uint64_t> child_rows;
};

/// Whether a member is a nested described type (Kind::kRecord).
template <class M>
constexpr bool IsRecordMember() {
  return IsDescribed<M>::value && !HasFieldTraits<M>::value;
}

/// Whether a member is a list of behaviours (Kind::kPolymorphicList).
template <class M>
constexpr bool IsPolymorphicList() {
  if constexpr (HasFieldTraits<M>::value) {
    return FieldTraits<M>::kKind == Kind::kPolymorphicList;
  } else if constexpr (IsStdVector<M>::value) {
    return IsDescribed<typename Unpointer<typename M::value_type>::type>::value;
  } else {
    return false;
  }
}

/// The position of S in the tuple of types L.
template <class S, class L, std::size_t I = 0>
constexpr std::size_t IndexIn() {
  if constexpr (I == std::tuple_size_v<L>) {
    return I;
  } else if constexpr (std::is_same_v<S, std::tuple_element_t<I, L>>) {
    return I;
  } else {
    return IndexIn<S, L, I + 1>();
  }
}

/// What one table holds, from its type's description: the columns in the
/// order the visitor hands fields over, with a nested record's fields in
/// place of the record and named by path ("inner/x"), and for each list of
/// behaviours the tables of its subtypes, in the order of the base's
/// Subtypes.
struct TableSpec {
  std::string type;  ///< the qualified name of the table's type
  TypeId id = 0;     ///< its identifier
  std::vector<TypeDescription> types;     ///< its chain and nested records
  std::vector<FieldDescription> columns;  ///< checkpoint fields, flattened
  std::vector<std::vector<TableSpec>> children;  ///< per column
};

template <class T>
void ListSpec(TableSpec* spec, const std::string& prefix);
template <class T>
TableSpec SpecOf();

template <class Base, std::size_t... I>
void SubtypeSpecs(std::vector<TableSpec>* out, std::index_sequence<I...>) {
  (out->push_back(SpecOf<std::tuple_element_t<I, SubtypesOf<Base>>>()), ...);
}

class SpecLister {
 public:
  SpecLister(TableSpec* spec, const std::string& prefix)
      : spec_(spec), prefix_(prefix) {}

  template <class M, class C>
  void operator()(const Field& f, M C::* member) {
    if (!HasRole(f.roles, Role::kCheckpoint)) {
      return;
    }
    const std::string name = prefix_ + std::string(f.name);
    if constexpr (IsRecordMember<M>()) {
      ListSpec<M>(spec_, name + "/");
    } else {
      std::vector<FieldDescription> one;
      Describer describe(&one);
      describe(f, member);
      one.front().name = name;
      spec_->columns.push_back(one.front());
      spec_->children.emplace_back();
      if constexpr (IsPolymorphicList<M>()) {
        using Base = typename ListOf<M>::Base;
        CheckSubtypes<Base>();
        SubtypeSpecs<Base>(
            &spec_->children.back(),
            std::make_index_sequence<std::tuple_size_v<SubtypesOf<Base>>>());
      }
    }
  }

 private:
  TableSpec* spec_;
  std::string prefix_;
};

template <class T>
void ListSpec(TableSpec* spec, const std::string& prefix) {
  using Base = typename TypeTraits<T>::Base;
  if constexpr (!std::is_void_v<Base>) {
    ListSpec<Base>(spec, prefix);
  }
  TypeDescription d = Describe<T>();
  bool known = false;
  for (const TypeDescription& t : spec->types) {
    known = known || t.id == d.id;
  }
  if (!known) {
    spec->types.push_back(d);
  }
  SpecLister lister(spec, prefix);
  TypeTraits<T>::Visit(lister);
}

template <class T>
TableSpec SpecOf() {
  TableSpec spec;
  spec.type = TypeTraits<T>::kName;
  spec.id = IdOf<T>();
  ListSpec<T>(&spec, "");
  return spec;
}

/// A block shaped like `spec`, empty.
inline void AllocateBlock(const TableSpec& spec,
                          std::vector<ColumnBuffer>* block) {
  block->assign(spec.columns.size(), ColumnBuffer());
  for (std::size_t i = 0; i < spec.columns.size(); ++i) {
    const std::vector<TableSpec>& kids = spec.children[i];
    (*block)[i].children.resize(kids.size());
    (*block)[i].child_rows.assign(kids.size(), 0);
    for (std::size_t k = 0; k < kids.size(); ++k) {
      AllocateBlock(kids[k], &(*block)[i].children[k]);
    }
  }
}

/// Empties a block, keeping its shape.
inline void ClearBlock(std::vector<ColumnBuffer>* block) {
  for (ColumnBuffer& c : *block) {
    c.bytes.clear();
    c.strings.clear();
    c.lengths.clear();
    for (std::size_t k = 0; k < c.children.size(); ++k) {
      ClearBlock(&c.children[k]);
      c.child_rows[k] = 0;
    }
  }
}

/// Whether a member is a variable-size array of elements (Kind::kVector).
template <class M>
constexpr bool IsVectorOfElements() {
  if constexpr (HasFieldTraits<M>::value) {
    return FieldTraits<M>::kKind == Kind::kVector;
  } else if constexpr (IsStdVector<M>::value) {
    return kIsElement<typename M::value_type>;
  } else {
    return false;
  }
}

struct ByteAppender {
  std::vector<char>* out;
  template <class X>
  void operator()(X& x) {
    const char* p = reinterpret_cast<const char*>(&x);
    out->insert(out->end(), p, p + sizeof(X));
  }
};

/// Receives the fields of one object after another and appends each to its
/// column's buffer.
class BlockCollector {
 public:
  explicit BlockCollector(std::vector<ColumnBuffer>* columns)
      : columns_(columns) {}
  void NextObject() { next_ = 0; }
  bool overflowed() const { return overflowed_; }
  /// A sentence when a list held an element that cannot be written.
  const std::string& error() const { return error_; }

  template <class M>
  void operator()(const Field& f, M& member) {
    if constexpr (IsRecordMember<M>()) {
      // A nested record's fields take their own columns, in place of it.
      VisitFields<M>(*this, member, Role::kCheckpoint);
      return;
    }
    if (next_ >= columns_->size()) {
      overflowed_ = true;
      return;
    }
    ColumnBuffer& column = (*columns_)[next_++];
    if constexpr (IsRecordMember<M>()) {
      // handled above
    } else if constexpr (std::is_same_v<M, std::string>) {
      column.strings.push_back(member);
    } else if constexpr (IsPolymorphicList<M>()) {
      using Subtypes = SubtypesOf<typename ListOf<M>::Base>;
      ByteAppender append{&column.bytes};
      uint64_t count = 0;
      std::string why;
      const bool ok = ForEachElement(
          member,
          [&](TypeId id, auto& element) {
            using S = std::remove_reference_t<decltype(element)>;
            constexpr std::size_t k = IndexIn<S, Subtypes>();
            if (k >= column.children.size()) {
              overflowed_ = true;
              return;
            }
            uint64_t stored = id;
            append(stored);
            BlockCollector sub(&column.children[k]);
            sub.NextObject();
            VisitFields<S>(sub, element, Role::kCheckpoint);
            overflowed_ = overflowed_ || sub.overflowed();
            if (!sub.error().empty()) {
              error_ = sub.error();
            }
            ++column.child_rows[k];
            ++count;
          },
          &why);
      if (!ok) {
        error_ = std::string(f.name) + ": " + why;
      }
      column.lengths.push_back(count);
    } else if constexpr (IsVectorOfElements<M>()) {
      ByteAppender append{&column.bytes};
      if constexpr (HasFieldTraits<M>::value) {
        using Traits = FieldTraits<M>;
        const std::size_t size = Traits::Size(member);
        for (std::size_t i = 0; i < size; ++i) {
          typename Traits::Element element = Traits::At(member, i);
          append(element);
        }
        column.lengths.push_back(size);
      } else {
        // By value, so that std::vector<bool>'s proxies work too.
        for (std::size_t i = 0; i < member.size(); ++i) {
          typename M::value_type element = member[i];
          append(element);
        }
        column.lengths.push_back(member.size());
      }
    } else {
      ByteAppender append{&column.bytes};
      PerElement<ByteAppender> elements(append);
      elements(f, member);
    }
  }

 private:
  std::vector<ColumnBuffer>* columns_;
  std::size_t next_ = 0;
  bool overflowed_ = false;
  std::string error_;
};

/// Takes elements from one column's bytes, in the order ByteAppender put them.
struct ByteTaker {
  const std::vector<char>* in;
  std::size_t* at;
  bool* short_of_bytes;
  template <class X>
  void operator()(X& x) {
    if (in->size() - *at < sizeof(X)) {
      *short_of_bytes = true;
      return;
    }
    if constexpr (std::is_same_v<X, bool>) {
      // Any byte but 0 is true, so a damaged file cannot make an invalid bool.
      x = (*in)[*at] != 0;
    } else {
      std::memcpy(&x, in->data() + *at, sizeof(X));
    }
    *at += sizeof(X);
  }
};

/// Hands the fields of one object after another their values from a block,
/// the reverse of BlockCollector.
class BlockScatter {
 public:
  explicit BlockScatter(const std::vector<ColumnBuffer>* columns)
      : columns_(columns), at_(columns->size(), 0) {}
  void NextObject() {
    if (started_) {
      ++row_;
    }
    started_ = true;
    next_ = 0;
  }
  /// Whether the block and the fields failed to match, which the reader's
  /// checks are meant to make impossible.
  bool mismatched() const {
    bool any = mismatched_;
    for (const auto& child : children_) {
      any = any || child.second->mismatched();
    }
    return any;
  }
  /// A sentence when a list could not be built, as for a behaviour type this
  /// build does not know.
  const std::string& error() const { return error_; }

  template <class M>
  void operator()(const Field& f, M& member) {
    if constexpr (IsRecordMember<M>()) {
      VisitFields<M>(*this, member, Role::kCheckpoint);
      return;
    }
    if (next_ >= columns_->size()) {
      mismatched_ = true;
      return;
    }
    const std::size_t c = next_++;
    const ColumnBuffer& column = (*columns_)[c];
    ByteTaker take{&column.bytes, &at_[c], &mismatched_};
    if constexpr (IsRecordMember<M>()) {
      // handled above
    } else if constexpr (IsPolymorphicList<M>()) {
      using Access = ListOf<M>;
      using Subtypes = SubtypesOf<typename Access::Base>;
      if (row_ >= column.lengths.size()) {
        mismatched_ = true;
        return;
      }
      if (Access::Size(member) != 0) {
        error_ = std::string(f.name) +
                 " already holds elements; a list is read into an empty one";
        return;
      }
      const uint64_t count = column.lengths[row_];
      for (uint64_t i = 0; i < count && error_.empty(); ++i) {
        uint64_t id = 0;
        take(id);
        std::string why;
        const bool ok = AppendElement(
            member, id,
            [&](auto& element) {
              using S = std::remove_reference_t<decltype(element)>;
              BlockScatter& sub = Child(c, IndexIn<S, Subtypes>());
              sub.NextObject();
              VisitFields<S>(sub, element, Role::kCheckpoint);
              if (!sub.error().empty()) {
                error_ = sub.error();
              }
            },
            &why);
        if (!ok) {
          error_ = std::string(f.name) + ": " + why;
        }
      }
    } else if constexpr (std::is_same_v<M, std::string>) {
      if (row_ >= column.strings.size()) {
        mismatched_ = true;
        return;
      }
      member = column.strings[row_];
    } else if constexpr (IsVectorOfElements<M>()) {
      if (row_ >= column.lengths.size()) {
        mismatched_ = true;
        return;
      }
      const std::size_t size = static_cast<std::size_t>(column.lengths[row_]);
      if constexpr (HasFieldTraits<M>::value) {
        using Traits = FieldTraits<M>;
        Traits::Resize(member, size);
        for (std::size_t i = 0; i < size; ++i) {
          typename Traits::Element element{};
          take(element);
          Traits::At(member, i) = element;
        }
      } else {
        member.resize(size);
        for (std::size_t i = 0; i < size; ++i) {
          typename M::value_type element{};
          take(element);
          member[i] = element;
        }
      }
    } else {
      PerElement<ByteTaker> elements(take);
      elements(f, member);
    }
  }

 private:
  // The scatter of subtype k of the list in column c, made on first use and
  // kept for the block, so that its rows are taken in turn.
  BlockScatter& Child(std::size_t c, std::size_t k) {
    std::unique_ptr<BlockScatter>& child = children_[{c, k}];
    if (!child) {
      static const std::vector<ColumnBuffer> kNone;
      const ColumnBuffer& column = (*columns_)[c];
      child = std::make_unique<BlockScatter>(
          k < column.children.size() ? &column.children[k] : &kNone);
    }
    return *child;
  }

  const std::vector<ColumnBuffer>* columns_;
  std::vector<std::size_t> at_;
  std::size_t row_ = 0;
  std::size_t next_ = 0;
  bool started_ = false;
  bool mismatched_ = false;
  std::string error_;
  std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<BlockScatter>>
      children_;
};

/// What a table records about its type, beside the columns, so that a resume
/// can refuse a model that changed: each type's version, base first, as
/// "name=version;name=version", and each checkpoint field's unit, as
/// "field=unit;field=unit", for fields that have one.
std::string VersionsOf(const TableSpec& spec);
std::string UnitsOf(const TableSpec& spec);

/// The descriptions of T's base types, outermost first, then T's own.
template <class T>
void DescribeChain(std::vector<TypeDescription>* out) {
  using Base = typename TypeTraits<T>::Base;
  if constexpr (!std::is_void_v<Base>) {
    DescribeChain<Base>(out);
  }
  out->push_back(Describe<T>());
}

}  // namespace detail

class CheckpointWriter {
 public:
  CheckpointWriter();
  ~CheckpointWriter();
  CheckpointWriter(const CheckpointWriter&) = delete;
  CheckpointWriter& operator=(const CheckpointWriter&) = delete;

  /// Creates the file. `why` receives a sentence on failure, here and below.
  Status Open(const std::string& path, const CheckpointOptions& options,
              std::string* why);

  /// Writes `count` objects of the described type T as the table at `path`,
  /// for example "agents/bdm::Cell". `at(i)` returns a reference to object i;
  /// objects are asked for once each, in order.
  template <class T, class At>
  Status WriteTable(const std::string& path, uint64_t count, At&& at,
                    std::string* why) {
    const detail::TableSpec spec = detail::SpecOf<T>();
    Table* table = nullptr;
    Status s = BeginTable(path, spec, &table, why);
    if (s != Status::kOk) {
      return s;
    }
    std::vector<detail::ColumnBuffer> block;
    detail::AllocateBlock(spec, &block);
    const uint64_t chunk = chunk_rows();
    for (uint64_t first = 0; first < count; first += chunk) {
      const uint64_t rows = count - first < chunk ? count - first : chunk;
      detail::ClearBlock(&block);
      detail::BlockCollector collect(&block);
      for (uint64_t i = 0; i < rows; ++i) {
        collect.NextObject();
        VisitFields<T>(collect, at(first + i), Role::kCheckpoint);
      }
      if (!collect.error().empty()) {
        return Refuse(path, collect.error(), why);
      }
      s = AppendBlock(table, block, rows, collect.overflowed(), why);
      if (s != Status::kOk) {
        return s;
      }
    }
    return Status::kOk;
  }

  /// Writes one object as the record at `path`: a table with one row.
  template <class T>
  Status WriteRecord(const std::string& path, const T& record,
                     std::string* why) {
    T copy = record;
    return WriteTable<T>(
        path, 1, [&copy](uint64_t) -> T& { return copy; }, why);
  }

  /// Writes the schema of everything written and closes the file.
  Status Close(std::string* why);

 private:
  struct Impl;
  struct Table;
  Status BeginTable(const std::string& path, const detail::TableSpec& spec,
                    Table** table, std::string* why);
  Status CreateTable(const std::string& path, const detail::TableSpec& spec,
                     std::unique_ptr<Table>* table, std::string* why);
  uint64_t chunk_rows() const;
  Status Refuse(const std::string& path, const std::string& problem,
                std::string* why);
  Status AppendBlock(Table* table, std::vector<detail::ColumnBuffer>& block,
                     uint64_t rows, bool overflowed, std::string* why);
  std::unique_ptr<Impl> impl_;
};

/// What the file's root says it is. The version is raised whenever the layout
/// changes, and a reader refuses a version it does not know.
constexpr const char* kCheckpointFormatName = "abm-io checkpoint";
constexpr uint32_t kCheckpointFormat = 1;

/// Where a checkpoint keeps what every simulation has (docs/INTEGRATION.md,
/// "Checkpoint files").
constexpr const char* kSimulationRecord = "records/abmio.Simulation";
constexpr const char* kComponentsTable = "components";
constexpr const char* kLineageTable = "lineage";

/// The components whose changes the caller accepts for a resume, by name, and
/// who accepts them and why (docs/RATIONALE.md, entry 11).
struct ResumeAcceptance {
  std::vector<std::string> components;  ///< exact names; no wildcard exists
  std::string accepted_by;              ///< required when anything is named
  std::string reason;                   ///< required when anything is named
  std::string at;                       ///< ISO 8601, UTC
};

/// Reads what CheckpointWriter wrote.
///
/// Strict, as resuming a simulation must be (docs/INTEGRATION.md, "Schema
/// evolution"): a table loads only if its fields with the checkpoint role are,
/// by name, kind, element type, count and unit, exactly those of the model's
/// type, and every type in its chain has the model's version; the failure
/// names what differs. CheckResume compares the components. Reading an older
/// file for analysis, with defaults for missing fields, is not offered yet.
/// Offsets of variable-size fields are checked before use: the first is 0, none
/// is smaller than the one before, and the last is the number of values.
class CheckpointReader {
 public:
  CheckpointReader();
  ~CheckpointReader();
  CheckpointReader(const CheckpointReader&) = delete;
  CheckpointReader& operator=(const CheckpointReader&) = delete;

  /// Opens the file for reading; of `options`, only chunk_rows counts here,
  /// the objects read per block. `why` receives a sentence on failure, here
  /// and below.
  Status Open(const std::string& path, const CheckpointOptions& options,
              std::string* why);

  /// The number of objects of type T in the table at `path`, after checking
  /// that the table matches T; the count to make room for before ReadTable.
  template <class T>
  Status Rows(const std::string& path, uint64_t* rows, std::string* why) {
    Table* table = nullptr;
    Status s = BeginTable(path, detail::SpecOf<T>(), &table, why);
    if (s == Status::kOk) {
      *rows = RowsOf(table);
    }
    EndTable();
    return s;
  }

  /// Reads the table at `path` into `count` objects of type T, which must be
  /// the number it holds. `at(i)` returns a reference to object i; objects are
  /// asked for once each, in order, a block at a time.
  template <class T, class At>
  Status ReadTable(const std::string& path, uint64_t count, At&& at,
                   std::string* why) {
    const detail::TableSpec spec = detail::SpecOf<T>();
    Table* table = nullptr;
    Status s = BeginTable(path, spec, &table, why);
    if (s == Status::kOk && count != RowsOf(table)) {
      s = CountDiffers(path, count, RowsOf(table), why);
    }
    if (s != Status::kOk) {
      EndTable();
      return s;
    }
    std::vector<detail::ColumnBuffer> block;
    detail::AllocateBlock(spec, &block);
    const uint64_t chunk = chunk_rows();
    for (uint64_t first = 0; s == Status::kOk && first < count;
         first += chunk) {
      const uint64_t rows = count - first < chunk ? count - first : chunk;
      detail::ClearBlock(&block);
      s = ReadBlock(table, first, rows, block, why);
      if (s != Status::kOk) {
        break;
      }
      detail::BlockScatter scatter(&block);
      for (uint64_t i = 0; i < rows && scatter.error().empty(); ++i) {
        scatter.NextObject();
        VisitFields<T>(scatter, at(first + i), Role::kCheckpoint);
      }
      if (!scatter.error().empty()) {
        s = Refuse(path, scatter.error(), why);
      } else if (scatter.mismatched()) {
        s = BlockMismatch(path, why);
      }
    }
    if (s == Status::kOk) {
      s = FinishTable(table, why);
    }
    EndTable();
    return s;
  }

  /// Reads the record at `path`, a table with one row, into `record`.
  template <class T>
  Status ReadRecord(const std::string& path, T* record, std::string* why) {
    return ReadTable<T>(
        path, 1, [record](uint64_t) -> T& { return *record; }, why);
  }

  /// The strict resume's check of dependencies (docs/INTEGRATION.md,
  /// "Provenance"): compares the components the checkpoint records with
  /// `now`. Every difference that affects results and is not accepted by name
  /// stops the resume with kBadSchema, all of them named in `why`. Accepted
  /// differences become lineage rows at the checkpoint's step; `lineage`
  /// receives the checkpoint's lineage followed by them, for the caller to
  /// write into every later checkpoint. Naming a component that did not
  /// change records nothing.
  Status CheckResume(const std::vector<ComponentRecord>& now,
                     const ResumeAcceptance& acceptance,
                     std::vector<LineageRecord>* lineage, std::string* why);

 private:
  struct Impl;
  struct Table;
  Status BeginTable(const std::string& path, const detail::TableSpec& spec,
                    Table** table, std::string* why);
  Status OpenTable(const std::string& path, const detail::TableSpec& spec,
                   std::unique_ptr<Table>* table, std::string* why);
  void EndTable();
  uint64_t RowsOf(const Table* table) const;
  uint64_t chunk_rows() const;
  Status FinishTable(Table* table, std::string* why);
  Status Refuse(const std::string& path, const std::string& problem,
                std::string* why);
  Status ReadBlock(Table* table, uint64_t first, uint64_t rows,
                   std::vector<detail::ColumnBuffer>& block, std::string* why);
  Status CountDiffers(const std::string& path, uint64_t count, uint64_t rows,
                      std::string* why);
  Status BlockMismatch(const std::string& path, std::string* why);
  std::unique_ptr<Impl> impl_;
};

}  // namespace abmio
