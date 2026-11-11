// SPDX-License-Identifier: Apache-2.0
// The checkpoint container's storage layer: typed, extendable columns in an
// HDF5 file, through HDF5's C API (docs/RATIONALE.md, entry 12).
//
// Internal to the library: the container is built on it, and nothing here is
// part of abm-io's interface. A column holds rows of `count` elements of one
// ElementType, grows by appending blocks, and is read back whole or by range,
// so neither side ever needs more than one block in memory
// (docs/INTEGRATION.md, "Memory while writing is bounded by the chunk").
//
// Files are written the same way every time: fixed little-endian types,
// unwritten parts of a chunk filled, and no modification times, so that two
// writes of the same data give the same bytes.
#pragma once
#include <hdf5.h>

#include <cstdint>
#include <string>
#include <vector>

#include "abmio/schema.h"
#include "abmio/status.h"

namespace abmio {
namespace h5 {

/// An HDF5 identifier that closes itself.
class Id {
 public:
  using Closer = herr_t (*)(hid_t);
  Id() = default;
  Id(hid_t id, Closer closer) : id_(id), closer_(closer) {}
  ~Id() { Reset(); }
  Id(const Id&) = delete;
  Id& operator=(const Id&) = delete;
  Id(Id&& other) noexcept : id_(other.id_), closer_(other.closer_) {
    other.id_ = -1;
  }
  Id& operator=(Id&& other) noexcept {
    if (this != &other) {
      Reset();
      id_ = other.id_;
      closer_ = other.closer_;
      other.id_ = -1;
    }
    return *this;
  }
  bool ok() const { return id_ >= 0; }
  hid_t get() const { return id_; }
  void Reset() {
    if (id_ >= 0 && closer_ != nullptr) {
      closer_(id_);
    }
    id_ = -1;
  }

 private:
  hid_t id_ = -1;
  Closer closer_ = nullptr;
};

/// How a column is stored.
struct ColumnLayout {
  uint64_t chunk_rows = 65536;  ///< rows per chunk, and so per block in memory
  bool lz4 = true;              ///< the registered LZ4 filter (abmio/h5lz4.h)
};

/// Creates a file, replacing one of the same name. `why` receives a sentence
/// on failure, here and in every function below.
Status CreateFile(const std::string& path, Id* file, std::string* why);
Status OpenFile(const std::string& path, Id* file, std::string* why);

/// Opens the group at `path` below `parent`, creating it and any group on the
/// way when `create` is set.
Status Group(hid_t parent, const std::string& path, bool create, Id* group,
             std::string* why);

Status WriteAttribute(hid_t object, const char* name, const std::string& value,
                      std::string* why);
Status ReadAttribute(hid_t object, const char* name, std::string* value,
                     std::string* why);

/// Rows of `count` elements of one type.
class Column {
 public:
  static Status Create(hid_t group, const std::string& name,
                       ElementType element, uint32_t count,
                       const ColumnLayout& layout, Column* out,
                       std::string* why);
  static Status Open(hid_t group, const std::string& name, Column* out,
                     std::string* why);

  /// Appends `rows` rows from `data`, which holds rows × count elements in
  /// the machine's own representation.
  Status Append(const void* data, uint64_t rows, std::string* why);
  /// Reads rows [first, first + rows) into `out`.
  Status Read(uint64_t first, uint64_t rows, void* out, std::string* why) const;

  uint64_t rows() const { return rows_; }
  ElementType element() const { return element_; }
  uint32_t count() const { return count_; }
  bool lz4() const { return lz4_; }

 private:
  Id set_;
  ElementType element_ = ElementType::kNone;
  uint32_t count_ = 1;
  uint64_t rows_ = 0;
  bool lz4_ = false;
};

/// Rows of one variable-length UTF-8 string each.
class StringColumn {
 public:
  static Status Create(hid_t group, const std::string& name,
                       uint64_t chunk_rows, StringColumn* out,
                       std::string* why);
  static Status Open(hid_t group, const std::string& name, StringColumn* out,
                     std::string* why);
  Status Append(const std::vector<std::string>& values, std::string* why);
  Status Read(uint64_t first, uint64_t rows, std::vector<std::string>* out,
              std::string* why) const;
  uint64_t rows() const { return rows_; }

 private:
  Id set_;
  uint64_t rows_ = 0;
};

}  // namespace h5
}  // namespace abmio
