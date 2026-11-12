// SPDX-License-Identifier: Apache-2.0
// Writing a checkpoint (docs/INTEGRATION.md, "Checkpoint files").
//
// A checkpoint holds tables, many objects of a described type, and records,
// exactly one; a record is a table with one row. Objects are walked through
// the visitor protocol a block at a time, so the writer's memory is bounded by
// fields × chunk and does not grow with the number of objects.
//
// Not yet written by this version, and refused with a message naming the
// field rather than stored in a layout nobody agreed: variable-size arrays,
// nested records, lists of behaviours, and fields declared StoredAs a
// narrower type. Nor does it yet write under a temporary name with a
// manifest; a file is complete only once Close has returned kOk.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

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
  std::vector<char> bytes;  ///< fixed-size fields, element after element
  std::vector<std::string> strings;  ///< string fields
};

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

}  // namespace abmio
