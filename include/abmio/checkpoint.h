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
// Not yet written by this version, and refused with a message naming the
// field rather than stored in a layout nobody agreed: nested records, lists of
// behaviours, and fields declared StoredAs a narrower type.
//
// The file is written as <path>.tmp and renamed to <path> in Close, after
// being flushed to disk, so it never appears half-written; a store of
// checkpoints with a manifest and a pointer is in abmio/store.h.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
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
  std::vector<uint64_t> lengths;     ///< variable-size arrays, one per object
};

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

  template <class M>
  void operator()(const Field& f, M& member) {
    if (next_ >= columns_->size()) {
      overflowed_ = true;
      return;
    }
    ColumnBuffer& column = (*columns_)[next_++];
    if constexpr (std::is_same_v<M, std::string>) {
      column.strings.push_back(member);
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
  bool mismatched() const { return mismatched_; }

  template <class M>
  void operator()(const Field& f, M& member) {
    if (next_ >= columns_->size()) {
      mismatched_ = true;
      return;
    }
    const std::size_t c = next_++;
    const ColumnBuffer& column = (*columns_)[c];
    ByteTaker take{&column.bytes, &at_[c], &mismatched_};
    if constexpr (std::is_same_v<M, std::string>) {
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
  const std::vector<ColumnBuffer>* columns_;
  std::vector<std::size_t> at_;
  std::size_t row_ = 0;
  std::size_t next_ = 0;
  bool started_ = false;
  bool mismatched_ = false;
};

/// What a table records about its type, beside the columns, so that a resume
/// can refuse a model that changed: each type's version, base first, as
/// "name=version;name=version", and each checkpoint field's unit, as
/// "field=unit;field=unit", for fields that have one.
std::string VersionsOf(const std::vector<TypeDescription>& chain);
std::string UnitsOf(const std::vector<TypeDescription>& chain);

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
    std::vector<TypeDescription> chain;
    detail::DescribeChain<T>(&chain);
    Table* table = nullptr;
    Status s = BeginTable(path, chain, &table, why);
    if (s != Status::kOk) {
      return s;
    }
    std::vector<detail::ColumnBuffer> block(ColumnsOf(table));
    const uint64_t chunk = chunk_rows();
    for (uint64_t first = 0; first < count; first += chunk) {
      const uint64_t rows = count - first < chunk ? count - first : chunk;
      for (detail::ColumnBuffer& column : block) {
        column.bytes.clear();
        column.strings.clear();
        column.lengths.clear();
      }
      detail::BlockCollector collect(&block);
      for (uint64_t i = 0; i < rows; ++i) {
        collect.NextObject();
        VisitFields<T>(collect, at(first + i), Role::kCheckpoint);
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
  Status BeginTable(const std::string& path,
                    const std::vector<TypeDescription>& chain, Table** table,
                    std::string* why);
  std::size_t ColumnsOf(const Table* table) const;
  uint64_t chunk_rows() const;
  Status AppendBlock(Table* table, std::vector<detail::ColumnBuffer>& block,
                     uint64_t rows, bool overflowed, std::string* why);
  std::unique_ptr<Impl> impl_;
};

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
    std::vector<TypeDescription> chain;
    detail::DescribeChain<T>(&chain);
    Table* table = nullptr;
    Status s = BeginTable(path, chain, &table, why);
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
    std::vector<TypeDescription> chain;
    detail::DescribeChain<T>(&chain);
    Table* table = nullptr;
    Status s = BeginTable(path, chain, &table, why);
    if (s == Status::kOk && count != RowsOf(table)) {
      s = CountDiffers(path, count, RowsOf(table), why);
    }
    if (s != Status::kOk) {
      EndTable();
      return s;
    }
    std::vector<detail::ColumnBuffer> block(ColumnsOf(table));
    const uint64_t chunk = chunk_rows();
    for (uint64_t first = 0; s == Status::kOk && first < count;
         first += chunk) {
      const uint64_t rows = count - first < chunk ? count - first : chunk;
      s = ReadBlock(table, first, rows, block, why);
      if (s != Status::kOk) {
        break;
      }
      detail::BlockScatter scatter(&block);
      for (uint64_t i = 0; i < rows; ++i) {
        scatter.NextObject();
        VisitFields<T>(scatter, at(first + i), Role::kCheckpoint);
      }
      if (scatter.mismatched()) {
        s = BlockMismatch(path, why);
      }
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
  Status BeginTable(const std::string& path,
                    const std::vector<TypeDescription>& chain, Table** table,
                    std::string* why);
  void EndTable();
  uint64_t RowsOf(const Table* table) const;
  std::size_t ColumnsOf(const Table* table) const;
  uint64_t chunk_rows() const;
  Status ReadBlock(Table* table, uint64_t first, uint64_t rows,
                   std::vector<detail::ColumnBuffer>& block, std::string* why);
  Status CountDiffers(const std::string& path, uint64_t count, uint64_t rows,
                      std::string* why);
  Status BlockMismatch(const std::string& path, std::string* why);
  std::unique_ptr<Impl> impl_;
};

}  // namespace abmio
