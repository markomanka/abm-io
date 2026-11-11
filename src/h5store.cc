// SPDX-License-Identifier: Apache-2.0
#include "h5store.h"

#include <cstring>

#include "abmio/h5lz4.h"

namespace abmio {
namespace h5 {
namespace {

// The element a column holds is recorded beside it: an agent identifier and a
// 64-bit unsigned integer are the same bytes, and only this tells them apart.
constexpr const char* kElementAttribute = "abmio_element";

Status Fail(Status s, std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message;
  }
  return s;
}

// Booleans as h5py writes them: an enumeration over int8 named FALSE and TRUE,
// so that Python readers get bools rather than numbers.
Id BoolType(hid_t base) {
  Id t(H5Tenum_create(base), H5Tclose);
  int8_t v = 0;
  H5Tenum_insert(t.get(), "FALSE", &v);
  v = 1;
  H5Tenum_insert(t.get(), "TRUE", &v);
  return t;
}

Id StringType() {
  Id t(H5Tcopy(H5T_C_S1), H5Tclose);
  H5Tset_size(t.get(), H5T_VARIABLE);
  H5Tset_cset(t.get(), H5T_CSET_UTF8);
  return t;
}

// The type in the file is fixed and little-endian whatever the machine; the
// type in memory is the machine's own, and HDF5 converts between them.
Id TypeOf(ElementType e, bool in_file) {
  hid_t t = -1;
  switch (e) {
    case ElementType::kBool:
      return BoolType(in_file ? H5T_STD_I8LE : H5T_NATIVE_INT8);
    case ElementType::kChar:
    case ElementType::kInt8:
      t = in_file ? H5T_STD_I8LE : H5T_NATIVE_INT8;
      break;
    case ElementType::kUInt8:
      t = in_file ? H5T_STD_U8LE : H5T_NATIVE_UINT8;
      break;
    case ElementType::kInt16:
      t = in_file ? H5T_STD_I16LE : H5T_NATIVE_INT16;
      break;
    case ElementType::kUInt16:
      t = in_file ? H5T_STD_U16LE : H5T_NATIVE_UINT16;
      break;
    case ElementType::kInt32:
      t = in_file ? H5T_STD_I32LE : H5T_NATIVE_INT32;
      break;
    case ElementType::kUInt32:
      t = in_file ? H5T_STD_U32LE : H5T_NATIVE_UINT32;
      break;
    case ElementType::kInt64:
      t = in_file ? H5T_STD_I64LE : H5T_NATIVE_INT64;
      break;
    case ElementType::kUInt64:
    case ElementType::kAgentId:
      t = in_file ? H5T_STD_U64LE : H5T_NATIVE_UINT64;
      break;
    case ElementType::kFloat32:
      t = in_file ? H5T_IEEE_F32LE : H5T_NATIVE_FLOAT;
      break;
    case ElementType::kFloat64:
      t = in_file ? H5T_IEEE_F64LE : H5T_NATIVE_DOUBLE;
      break;
    case ElementType::kNone:
      return Id();
  }
  return Id(H5Tcopy(t), H5Tclose);
}

// No modification times in the file: they would make two writes of the same
// data differ.
void Untimed(hid_t creation_properties) {
  H5Pset_obj_track_times(creation_properties, 0);
}

bool Exists(hid_t group, const std::string& name) {
  return H5Lexists(group, name.c_str(), H5P_DEFAULT) > 0;
}

uint64_t RowsOf(hid_t set) {
  Id space(H5Dget_space(set), H5Sclose);
  hsize_t dims[2] = {0, 0};
  H5Sget_simple_extent_dims(space.get(), dims, nullptr);
  return dims[0];
}

}  // namespace

Status CreateFile(const std::string& path, Id* file, std::string* why) {
  *file = Id(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
             H5Fclose);
  return file->ok() ? Status::kOk
                    : Fail(Status::kIo, why, "cannot create " + path);
}

Status OpenFile(const std::string& path, Id* file, std::string* why) {
  if (H5Fis_hdf5(path.c_str()) <= 0) {
    return Fail(Status::kIo, why, path + " is missing or is not an HDF5 file");
  }
  *file = Id(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  return file->ok() ? Status::kOk
                    : Fail(Status::kIo, why, "cannot open " + path);
}

Status Group(hid_t parent, const std::string& path, bool create, Id* group,
             std::string* why) {
  if (Exists(parent, path)) {
    *group = Id(H5Gopen2(parent, path.c_str(), H5P_DEFAULT), H5Gclose);
  } else if (create) {
    Id lcpl(H5Pcreate(H5P_LINK_CREATE), H5Pclose);
    H5Pset_create_intermediate_group(lcpl.get(), 1);
    Id gcpl(H5Pcreate(H5P_GROUP_CREATE), H5Pclose);
    Untimed(gcpl.get());
    *group = Id(
        H5Gcreate2(parent, path.c_str(), lcpl.get(), gcpl.get(), H5P_DEFAULT),
        H5Gclose);
  } else {
    return Fail(Status::kCorrupt, why, "missing group " + path);
  }
  return group->ok() ? Status::kOk
                     : Fail(Status::kIo, why, "cannot open group " + path);
}

Status WriteAttribute(hid_t object, const char* name, const std::string& value,
                      std::string* why) {
  Id type = StringType();
  Id space(H5Screate(H5S_SCALAR), H5Sclose);
  Id attr(H5Acreate2(object, name, type.get(), space.get(), H5P_DEFAULT,
                     H5P_DEFAULT),
          H5Aclose);
  const char* p = value.c_str();
  if (!attr.ok() || H5Awrite(attr.get(), type.get(), &p) < 0) {
    return Fail(Status::kIo, why,
                std::string("cannot write attribute ") + name);
  }
  return Status::kOk;
}

Status ReadAttribute(hid_t object, const char* name, std::string* value,
                     std::string* why) {
  if (H5Aexists(object, name) <= 0) {
    return Fail(Status::kCorrupt, why,
                std::string("missing attribute ") + name);
  }
  Id attr(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  Id type = StringType();
  char* p = nullptr;
  if (!attr.ok() || H5Aread(attr.get(), type.get(), &p) < 0) {
    return Fail(Status::kIo, why, std::string("cannot read attribute ") + name);
  }
  value->assign(p == nullptr ? "" : p);
  H5free_memory(p);
  return Status::kOk;
}

Status Column::Create(hid_t group, const std::string& name, ElementType element,
                      uint32_t count, const ColumnLayout& layout, Column* out,
                      std::string* why) {
  if (element == ElementType::kNone || count == 0 || layout.chunk_rows == 0) {
    return Fail(Status::kBadSchema, why,
                "column " + name + " has no element type, count or chunk");
  }
  if (layout.lz4 && !RegisterLz4Filter()) {
    return Fail(Status::kIo, why, "HDF5 refused the LZ4 filter");
  }
  const int rank = count > 1 ? 2 : 1;
  const hsize_t dims[2] = {0, count};
  const hsize_t maxdims[2] = {H5S_UNLIMITED, count};
  const hsize_t chunk[2] = {layout.chunk_rows, count};
  Id space(H5Screate_simple(rank, dims, maxdims), H5Sclose);
  Id dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
  H5Pset_chunk(dcpl.get(), rank, chunk);
  // The last chunk is partial unless the rows are a multiple of the chunk,
  // and HDF5 leaves the unwritten part uninitialised unless told to fill on
  // allocation: without this, two writes of the same rows differ in bytes
  // nobody wrote.
  H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_ALLOC);
  Untimed(dcpl.get());
  if (layout.lz4) {
    H5Pset_filter(dcpl.get(), kLz4FilterId, H5Z_FLAG_MANDATORY, 0, nullptr);
  }
  // A cache of exactly one chunk: smaller, and every partial chunk becomes a
  // read, a modify and a write; larger buys nothing when blocks are appended
  // in order.
  Id dapl(H5Pcreate(H5P_DATASET_ACCESS), H5Pclose);
  H5Pset_chunk_cache(dapl.get(), 521,
                     layout.chunk_rows * count * SizeOf(element), 1.0);

  Id file_type = TypeOf(element, true);
  out->set_ = Id(H5Dcreate2(group, name.c_str(), file_type.get(), space.get(),
                            H5P_DEFAULT, dcpl.get(), dapl.get()),
                 H5Dclose);
  if (!out->set_.ok()) {
    return Fail(Status::kIo, why, "cannot create column " + name);
  }
  const uint8_t tag = static_cast<uint8_t>(element);
  Id scalar(H5Screate(H5S_SCALAR), H5Sclose);
  Id attr(H5Acreate2(out->set_.get(), kElementAttribute, H5T_STD_U8LE,
                     scalar.get(), H5P_DEFAULT, H5P_DEFAULT),
          H5Aclose);
  if (!attr.ok() || H5Awrite(attr.get(), H5T_NATIVE_UINT8, &tag) < 0) {
    return Fail(Status::kIo, why, "cannot label column " + name);
  }
  out->element_ = element;
  out->count_ = count;
  out->rows_ = 0;
  out->lz4_ = layout.lz4;
  return Status::kOk;
}

Status Column::Open(hid_t group, const std::string& name, Column* out,
                    std::string* why) {
  if (!Exists(group, name)) {
    return Fail(Status::kCorrupt, why, "missing column " + name);
  }
  // The filter must be known before a compressed chunk is read.
  RegisterLz4Filter();
  out->set_ = Id(H5Dopen2(group, name.c_str(), H5P_DEFAULT), H5Dclose);
  if (!out->set_.ok()) {
    return Fail(Status::kIo, why, "cannot open column " + name);
  }
  uint8_t tag = 0;
  if (H5Aexists(out->set_.get(), kElementAttribute) <= 0) {
    return Fail(Status::kCorrupt, why,
                "column " + name + " does not say what it holds");
  }
  Id attr(H5Aopen(out->set_.get(), kElementAttribute, H5P_DEFAULT), H5Aclose);
  if (H5Aread(attr.get(), H5T_NATIVE_UINT8, &tag) < 0 || tag == 0 ||
      tag > static_cast<uint8_t>(ElementType::kAgentId)) {
    return Fail(Status::kCorrupt, why,
                "column " + name + " holds an unknown element type");
  }
  out->element_ = static_cast<ElementType>(tag);

  Id space(H5Dget_space(out->set_.get()), H5Sclose);
  const int rank = H5Sget_simple_extent_ndims(space.get());
  hsize_t dims[2] = {0, 1};
  if (rank < 1 || rank > 2 ||
      H5Sget_simple_extent_dims(space.get(), dims, nullptr) < 0) {
    return Fail(Status::kCorrupt, why,
                "column " + name + " is not a table of rows");
  }
  out->rows_ = dims[0];
  out->count_ = rank == 2 ? static_cast<uint32_t>(dims[1]) : 1;

  // What is stored must be as wide as what the label says, or reading it
  // into a buffer sized by the label would be reading something else.
  Id stored(H5Dget_type(out->set_.get()), H5Tclose);
  if (H5Tget_size(stored.get()) != SizeOf(out->element_)) {
    return Fail(Status::kCorrupt, why,
                "column " + name + " is not stored as its label says");
  }
  Id dcpl(H5Dget_create_plist(out->set_.get()), H5Pclose);
  out->lz4_ = false;
  const int filters = H5Pget_nfilters(dcpl.get());
  for (int i = 0; i < filters; ++i) {
    unsigned flags = 0;
    size_t elements = 0;
    unsigned config = 0;
    if (H5Pget_filter2(dcpl.get(), static_cast<unsigned>(i), &flags, &elements,
                       nullptr, 0, nullptr,
                       &config) == static_cast<H5Z_filter_t>(kLz4FilterId)) {
      out->lz4_ = true;
    }
  }
  return Status::kOk;
}

Status Column::Append(const void* data, uint64_t rows, std::string* why) {
  if (rows == 0) {
    return Status::kOk;
  }
  const int rank = count_ > 1 ? 2 : 1;
  const hsize_t size[2] = {rows_ + rows, count_};
  if (H5Dset_extent(set_.get(), size) < 0) {
    return Fail(Status::kIo, why, "cannot extend a column");
  }
  Id file_space(H5Dget_space(set_.get()), H5Sclose);
  const hsize_t start[2] = {rows_, 0};
  const hsize_t block[2] = {rows, count_};
  H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, start, nullptr, block,
                      nullptr);
  Id mem_space(H5Screate_simple(rank, block, nullptr), H5Sclose);
  Id mem_type = TypeOf(element_, false);
  if (H5Dwrite(set_.get(), mem_type.get(), mem_space.get(), file_space.get(),
               H5P_DEFAULT, data) < 0) {
    return Fail(Status::kIo, why, "cannot write a block of a column");
  }
  rows_ += rows;
  return Status::kOk;
}

Status Column::Read(uint64_t first, uint64_t rows, void* out,
                    std::string* why) const {
  if (first > rows_ || rows > rows_ - first) {
    return Fail(Status::kBadIndex, why,
                "rows asked for lie beyond the end of the column");
  }
  if (rows == 0) {
    return Status::kOk;
  }
  const int rank = count_ > 1 ? 2 : 1;
  Id file_space(H5Dget_space(set_.get()), H5Sclose);
  const hsize_t start[2] = {first, 0};
  const hsize_t block[2] = {rows, count_};
  H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, start, nullptr, block,
                      nullptr);
  Id mem_space(H5Screate_simple(rank, block, nullptr), H5Sclose);
  Id mem_type = TypeOf(element_, false);
  if (H5Dread(set_.get(), mem_type.get(), mem_space.get(), file_space.get(),
              H5P_DEFAULT, out) < 0) {
    // A block the filter rejects arrives here: the file is damaged, not absent.
    return Fail(Status::kCorrupt, why, "cannot read a block of a column");
  }
  return Status::kOk;
}

Status StringColumn::Create(hid_t group, const std::string& name,
                            uint64_t chunk_rows, StringColumn* out,
                            std::string* why) {
  if (chunk_rows == 0) {
    return Fail(Status::kBadSchema, why, "column " + name + " has no chunk");
  }
  const hsize_t dims = 0;
  const hsize_t maxdims = H5S_UNLIMITED;
  const hsize_t chunk = chunk_rows;
  Id space(H5Screate_simple(1, &dims, &maxdims), H5Sclose);
  Id dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
  H5Pset_chunk(dcpl.get(), 1, &chunk);
  H5Pset_fill_time(dcpl.get(), H5D_FILL_TIME_ALLOC);
  Untimed(dcpl.get());
  Id type = StringType();
  out->set_ = Id(H5Dcreate2(group, name.c_str(), type.get(), space.get(),
                            H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
                 H5Dclose);
  out->rows_ = 0;
  return out->set_.ok()
             ? Status::kOk
             : Fail(Status::kIo, why, "cannot create column " + name);
}

Status StringColumn::Open(hid_t group, const std::string& name,
                          StringColumn* out, std::string* why) {
  if (!Exists(group, name)) {
    return Fail(Status::kCorrupt, why, "missing column " + name);
  }
  out->set_ = Id(H5Dopen2(group, name.c_str(), H5P_DEFAULT), H5Dclose);
  if (!out->set_.ok()) {
    return Fail(Status::kIo, why, "cannot open column " + name);
  }
  Id type(H5Dget_type(out->set_.get()), H5Tclose);
  if (H5Tget_class(type.get()) != H5T_STRING ||
      H5Tis_variable_str(type.get()) <= 0) {
    return Fail(Status::kCorrupt, why,
                "column " + name + " does not hold strings");
  }
  out->rows_ = RowsOf(out->set_.get());
  return Status::kOk;
}

Status StringColumn::Append(const std::vector<std::string>& values,
                            std::string* why) {
  if (values.empty()) {
    return Status::kOk;
  }
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const std::string& v : values) {
    pointers.push_back(v.c_str());
  }
  const hsize_t size = rows_ + values.size();
  if (H5Dset_extent(set_.get(), &size) < 0) {
    return Fail(Status::kIo, why, "cannot extend a column of strings");
  }
  Id file_space(H5Dget_space(set_.get()), H5Sclose);
  const hsize_t start = rows_;
  const hsize_t block = values.size();
  H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, &start, nullptr, &block,
                      nullptr);
  Id mem_space(H5Screate_simple(1, &block, nullptr), H5Sclose);
  Id type = StringType();
  if (H5Dwrite(set_.get(), type.get(), mem_space.get(), file_space.get(),
               H5P_DEFAULT, pointers.data()) < 0) {
    return Fail(Status::kIo, why, "cannot write a block of strings");
  }
  rows_ += values.size();
  return Status::kOk;
}

Status StringColumn::Read(uint64_t first, uint64_t rows,
                          std::vector<std::string>* out,
                          std::string* why) const {
  out->clear();
  if (first > rows_ || rows > rows_ - first) {
    return Fail(Status::kBadIndex, why,
                "rows asked for lie beyond the end of the column");
  }
  if (rows == 0) {
    return Status::kOk;
  }
  Id file_space(H5Dget_space(set_.get()), H5Sclose);
  const hsize_t start = first;
  const hsize_t block = rows;
  H5Sselect_hyperslab(file_space.get(), H5S_SELECT_SET, &start, nullptr, &block,
                      nullptr);
  Id mem_space(H5Screate_simple(1, &block, nullptr), H5Sclose);
  Id type = StringType();
  std::vector<char*> pointers(rows, nullptr);
  if (H5Dread(set_.get(), type.get(), mem_space.get(), file_space.get(),
              H5P_DEFAULT, pointers.data()) < 0) {
    return Fail(Status::kCorrupt, why, "cannot read a block of strings");
  }
  out->reserve(rows);
  for (char* p : pointers) {
    out->emplace_back(p == nullptr ? "" : p);
  }
  // The strings were allocated by HDF5 and are released through it; the call
  // was renamed in 1.12 and the old name is gone from HDF5 2.x.
#if H5_VERSION_GE(1, 12, 0)
  H5Treclaim(type.get(), mem_space.get(), H5P_DEFAULT, pointers.data());
#else
  H5Dvlen_reclaim(type.get(), mem_space.get(), H5P_DEFAULT, pointers.data());
#endif
  return Status::kOk;
}

}  // namespace h5
}  // namespace abmio
