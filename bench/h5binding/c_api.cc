// SPDX-License-Identifier: Apache-2.0
// The checkpoint writer and reader of bench/h5binding/binding.h on HDF5's C
// API alone.
#include <hdf5.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "abmio/h5lz4.h"
#include "binding.h"

namespace abmio {
namespace h5binding {
namespace {

// Every HDF5 identifier this file opens is closed by its owner going out of
// scope, whatever path the function leaves by.
class Id {
 public:
  using Closer = herr_t (*)(hid_t);
  Id(hid_t id, Closer closer) : id_(id), closer_(closer) {}
  ~Id() {
    if (id_ >= 0) {
      closer_(id_);
    }
  }
  Id(const Id&) = delete;
  Id& operator=(const Id&) = delete;
  Id(Id&& other) noexcept : id_(other.id_), closer_(other.closer_) {
    other.id_ = -1;
  }
  Id& operator=(Id&&) = delete;
  bool ok() const { return id_ >= 0; }
  operator hid_t() const { return id_; }

 private:
  hid_t id_;
  Closer closer_;
};

#define ABMIO_FAIL(why, message) \
  do {                           \
    *(why) = (message);          \
    return false;                \
  } while (0)

// A variable-length UTF-8 string, the type of every string in these files.
Id StringType() {
  Id t(H5Tcopy(H5T_C_S1), H5Tclose);
  H5Tset_size(t, H5T_VARIABLE);
  H5Tset_cset(t, H5T_CSET_UTF8);
  return t;
}

// Booleans as h5py writes them: an enumeration over int8 named FALSE and TRUE,
// so that Python readers get bools rather than numbers.
Id BoolType() {
  Id t(H5Tenum_create(H5T_NATIVE_INT8), H5Tclose);
  int8_t v = 0;
  H5Tenum_insert(t, "FALSE", &v);
  v = 1;
  H5Tenum_insert(t, "TRUE", &v);
  return t;
}

bool WriteStringAttribute(hid_t loc, const char* name, const std::string& s,
                          std::string* why) {
  Id type = StringType();
  Id space(H5Screate(H5S_SCALAR), H5Sclose);
  Id attr(H5Acreate2(loc, name, type, space, H5P_DEFAULT, H5P_DEFAULT),
          H5Aclose);
  const char* p = s.c_str();
  if (!attr.ok() || H5Awrite(attr, type, &p) < 0) {
    ABMIO_FAIL(why, std::string("cannot write attribute ") + name);
  }
  return true;
}

bool WriteScalarAttribute(hid_t loc, const char* name, uint32_t v,
                          std::string* why) {
  Id space(H5Screate(H5S_SCALAR), H5Sclose);
  Id attr(
      H5Acreate2(loc, name, H5T_NATIVE_UINT32, space, H5P_DEFAULT, H5P_DEFAULT),
      H5Aclose);
  if (!attr.ok() || H5Awrite(attr, H5T_NATIVE_UINT32, &v) < 0) {
    ABMIO_FAIL(why, std::string("cannot write attribute ") + name);
  }
  return true;
}

// One column of a small table, written whole: the records and tables beside
// the agents are a few rows, so they are contiguous and not chunked.
bool WriteColumn(hid_t group, const char* name, hid_t type, hsize_t rows,
                 const void* data, std::string* why) {
  Id space(H5Screate_simple(1, &rows, nullptr), H5Sclose);
  Id set(H5Dcreate2(group, name, type, space, H5P_DEFAULT, H5P_DEFAULT,
                    H5P_DEFAULT),
         H5Dclose);
  if (!set.ok() ||
      H5Dwrite(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) < 0) {
    ABMIO_FAIL(why, std::string("cannot write ") + name);
  }
  return true;
}

bool WriteStrings(hid_t group, const char* name,
                  const std::vector<std::string>& values, std::string* why) {
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const std::string& v : values) {
    pointers.push_back(v.c_str());
  }
  Id type = StringType();
  return WriteColumn(group, name, type, values.size(), pointers.data(), why);
}

bool WriteBools(hid_t group, const char* name, const std::vector<int8_t>& v,
                std::string* why) {
  Id type = BoolType();
  return WriteColumn(group, name, type, v.size(), v.data(), why);
}

// An agent column: extendable, chunked by `layout.chunk` agents, with the LZ4
// filter when asked, and a chunk cache sized as asked.
Id CreateAgentColumn(hid_t group, const char* name, hid_t type, hsize_t width,
                     const Layout& layout) {
  const int rank = width > 1 ? 2 : 1;
  hsize_t dims[2] = {0, width};
  hsize_t maxdims[2] = {H5S_UNLIMITED, width};
  hsize_t chunk[2] = {layout.chunk, width};
  Id space(H5Screate_simple(rank, dims, maxdims), H5Sclose);
  Id dcpl(H5Pcreate(H5P_DATASET_CREATE), H5Pclose);
  H5Pset_chunk(dcpl, rank, chunk);
  // The last chunk is partial unless the population is a multiple of the
  // chunk, and HDF5 leaves the unwritten part of a chunk uninitialised unless
  // told to fill on allocation; without this, two writes of the same agents
  // give files that differ in the bytes nobody wrote. Found by `cmp`.
  H5Pset_fill_time(dcpl, H5D_FILL_TIME_ALLOC);
  if (layout.lz4) {
    H5Pset_filter(dcpl, kLz4FilterId, H5Z_FLAG_MANDATORY, 0, nullptr);
  }
  Id dapl(H5Pcreate(H5P_DATASET_ACCESS), H5Pclose);
  if (layout.cache_bytes > 0) {
    H5Pset_chunk_cache(dapl, 521, layout.cache_bytes, 1.0);
  }
  return Id(H5Dcreate2(group, name, type, space, H5P_DEFAULT, dcpl, dapl),
            H5Dclose);
}

// Appends one block to an agent column: extend, select the new rows, write.
bool Append(hid_t set, hid_t type, hsize_t first, hsize_t count, hsize_t width,
            const void* data, std::string* why) {
  hsize_t size[2] = {first + count, width};
  if (H5Dset_extent(set, size) < 0) {
    ABMIO_FAIL(why, "cannot extend an agent column");
  }
  Id file_space(H5Dget_space(set), H5Sclose);
  hsize_t start[2] = {first, 0};
  hsize_t block[2] = {count, width};
  H5Sselect_hyperslab(file_space, H5S_SELECT_SET, start, nullptr, block,
                      nullptr);
  const int rank = width > 1 ? 2 : 1;
  Id mem_space(H5Screate_simple(rank, block, nullptr), H5Sclose);
  if (H5Dwrite(set, type, mem_space, file_space, H5P_DEFAULT, data) < 0) {
    ABMIO_FAIL(why, "cannot write an agent block");
  }
  return true;
}

bool ReadBlock(hid_t set, hid_t type, hsize_t first, hsize_t count,
               hsize_t width, void* data, std::string* why) {
  Id file_space(H5Dget_space(set), H5Sclose);
  hsize_t start[2] = {first, 0};
  hsize_t block[2] = {count, width};
  H5Sselect_hyperslab(file_space, H5S_SELECT_SET, start, nullptr, block,
                      nullptr);
  const int rank = width > 1 ? 2 : 1;
  Id mem_space(H5Screate_simple(rank, block, nullptr), H5Sclose);
  if (H5Dread(set, type, mem_space, file_space, H5P_DEFAULT, data) < 0) {
    ABMIO_FAIL(why, "cannot read an agent block");
  }
  return true;
}

bool ReadStrings(hid_t group, const char* name, std::vector<std::string>* out,
                 std::string* why) {
  Id set(H5Dopen2(group, name, H5P_DEFAULT), H5Dclose);
  if (!set.ok()) {
    ABMIO_FAIL(why, std::string("missing ") + name);
  }
  Id space(H5Dget_space(set), H5Sclose);
  hsize_t rows = 0;
  H5Sget_simple_extent_dims(space, &rows, nullptr);
  std::vector<char*> pointers(rows, nullptr);
  Id type = StringType();
  if (rows > 0 &&
      H5Dread(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, pointers.data()) < 0) {
    ABMIO_FAIL(why, std::string("cannot read ") + name);
  }
  out->clear();
  for (char* p : pointers) {
    out->emplace_back(p == nullptr ? "" : p);
  }
  // The strings were allocated by HDF5 and are released through it; the call
  // was renamed in 1.12 and the old name is gone from HDF5 2.x.
#if H5_VERSION_GE(1, 12, 0)
  H5Treclaim(type, space, H5P_DEFAULT, pointers.data());
#else
  H5Dvlen_reclaim(type, space, H5P_DEFAULT, pointers.data());
#endif
  return true;
}

template <class T>
bool ReadNumbers(hid_t group, const char* name, hid_t type, std::vector<T>* out,
                 std::string* why) {
  Id set(H5Dopen2(group, name, H5P_DEFAULT), H5Dclose);
  if (!set.ok()) {
    ABMIO_FAIL(why, std::string("missing ") + name);
  }
  Id space(H5Dget_space(set), H5Sclose);
  hsize_t rows = 0;
  H5Sget_simple_extent_dims(space, &rows, nullptr);
  out->resize(rows);
  if (rows > 0 &&
      H5Dread(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out->data()) < 0) {
    ABMIO_FAIL(why, std::string("cannot read ") + name);
  }
  return true;
}

class CApi : public Binding {
 public:
  const char* Name() const override { return "c-api"; }

  bool Write(const std::string& path, const Layout& layout,
             const Records& records, std::string* why) override {
    if (layout.lz4 && !RegisterLz4Filter()) {
      ABMIO_FAIL(why, "cannot register the LZ4 filter");
    }
    Id file(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
            H5Fclose);
    if (!file.ok()) {
      ABMIO_FAIL(why, "cannot create " + path);
    }
    if (!WriteScalarAttribute(file, "format_version", 1, why) ||
        !WriteStringAttribute(file, "producer", "abm-io h5binding", why)) {
      return false;
    }
    {
      Id type = StringType();
      const std::string schema = "{\"types\": []}";
      Id space(H5Screate(H5S_SCALAR), H5Sclose);
      Id set(H5Dcreate2(file, "schema", type, space, H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
             H5Dclose);
      const char* p = schema.c_str();
      if (!set.ok() ||
          H5Dwrite(set, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &p) < 0) {
        ABMIO_FAIL(why, "cannot write /schema");
      }
    }

    // Agents, one block per chunk.
    Id lcpl(H5Pcreate(H5P_LINK_CREATE), H5Pclose);
    H5Pset_create_intermediate_group(lcpl, 1);
    Id cell(H5Gcreate2(file, "/agents/cell", lcpl, H5P_DEFAULT, H5P_DEFAULT),
            H5Gclose);
    Id uid = CreateAgentColumn(cell, "uid", H5T_NATIVE_UINT64, 1, layout);
    Id position =
        CreateAgentColumn(cell, "position", H5T_NATIVE_DOUBLE, 3, layout);
    Id diameter =
        CreateAgentColumn(cell, "diameter", H5T_NATIVE_DOUBLE, 1, layout);
    Id cell_type =
        CreateAgentColumn(cell, "cell_type", H5T_NATIVE_INT32, 1, layout);
    Id mother = CreateAgentColumn(cell, "mother", H5T_NATIVE_UINT64, 1, layout);
    if (!uid.ok() || !position.ok() || !diameter.ok() || !cell_type.ok() ||
        !mother.ok()) {
      ABMIO_FAIL(why, "cannot create the agent columns");
    }
    Block block;
    for (uint64_t first = 0; first < layout.agents; first += layout.chunk) {
      const uint64_t n = layout.agents - first < layout.chunk
                             ? layout.agents - first
                             : layout.chunk;
      block.Fill(first, n);
      if (!Append(uid, H5T_NATIVE_UINT64, first, n, 1, block.uid.data(), why) ||
          !Append(position, H5T_NATIVE_DOUBLE, first, n, 3,
                  block.position.data(), why) ||
          !Append(diameter, H5T_NATIVE_DOUBLE, first, n, 1,
                  block.diameter.data(), why) ||
          !Append(cell_type, H5T_NATIVE_INT32, first, n, 1,
                  block.cell_type.data(), why) ||
          !Append(mother, H5T_NATIVE_UINT64, first, n, 1, block.mother.data(),
                  why)) {
        return false;
      }
    }

    // The record and the tables, whole.
    Id sim(H5Gcreate2(file, "/records/abmio.Simulation", lcpl, H5P_DEFAULT,
                      H5P_DEFAULT),
           H5Gclose);
    if (!WriteColumn(sim, "step", H5T_NATIVE_UINT64, 1,
                     &records.simulation.step, why) ||
        !WriteColumn(sim, "time", H5T_NATIVE_DOUBLE, 1,
                     &records.simulation.time, why) ||
        !WriteStrings(sim, "time_unit", {records.simulation.time_unit}, why)) {
      return false;
    }
    Id components(
        H5Gcreate2(file, "/components", lcpl, H5P_DEFAULT, H5P_DEFAULT),
        H5Gclose);
    std::vector<std::string> name, version, role, digest, reference;
    std::vector<int8_t> affects;
    for (const ComponentRecord& c : records.components) {
      name.push_back(c.name);
      version.push_back(c.version);
      role.push_back(c.role);
      digest.push_back(c.digest);
      reference.push_back(c.reference);
      affects.push_back(c.affects_results ? 1 : 0);
    }
    if (!WriteStrings(components, "name", name, why) ||
        !WriteStrings(components, "version", version, why) ||
        !WriteStrings(components, "role", role, why) ||
        !WriteStrings(components, "digest", digest, why) ||
        !WriteBools(components, "affects_results", affects, why) ||
        !WriteStrings(components, "reference", reference, why)) {
      return false;
    }
    Id random(H5Gcreate2(file, "/random", lcpl, H5P_DEFAULT, H5P_DEFAULT),
              H5Gclose);
    std::vector<std::string> owner;
    std::vector<uint64_t> key, counter;
    for (const RandomStreamRecord& r : records.streams) {
      owner.push_back(r.owner);
      key.push_back(r.key);
      counter.push_back(r.counter);
    }
    return WriteStrings(random, "owner", owner, why) &&
           WriteColumn(random, "key", H5T_NATIVE_UINT64, key.size(), key.data(),
                       why) &&
           WriteColumn(random, "counter", H5T_NATIVE_UINT64, counter.size(),
                       counter.data(), why);
  }

  bool Read(const std::string& path, uint64_t chunk, ReadResult* out,
            std::string* why) override {
    RegisterLz4Filter();
    Id file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
    if (!file.ok()) {
      ABMIO_FAIL(why, "cannot open " + path);
    }
    Id cell(H5Gopen2(file, "/agents/cell", H5P_DEFAULT), H5Gclose);
    Id uid(H5Dopen2(cell, "uid", H5P_DEFAULT), H5Dclose);
    Id position(H5Dopen2(cell, "position", H5P_DEFAULT), H5Dclose);
    Id diameter(H5Dopen2(cell, "diameter", H5P_DEFAULT), H5Dclose);
    Id cell_type(H5Dopen2(cell, "cell_type", H5P_DEFAULT), H5Dclose);
    Id mother(H5Dopen2(cell, "mother", H5P_DEFAULT), H5Dclose);
    if (!uid.ok() || !position.ok() || !diameter.ok() || !cell_type.ok() ||
        !mother.ok()) {
      ABMIO_FAIL(why, "missing agent columns");
    }
    {
      Id dcpl(H5Dget_create_plist(position), H5Pclose);
      const int n = H5Pget_nfilters(dcpl);
      for (int i = 0; i < n; ++i) {
        unsigned flags = 0;
        size_t nelmts = 0;
        out->filters.push_back(static_cast<unsigned>(
            H5Pget_filter2(dcpl, static_cast<unsigned>(i), &flags, &nelmts,
                           nullptr, 0, nullptr, nullptr)));
      }
    }
    Id space(H5Dget_space(uid), H5Sclose);
    hsize_t agents = 0;
    H5Sget_simple_extent_dims(space, &agents, nullptr);
    out->agents = agents;
    out->mismatches = 0;
    Block block;
    for (uint64_t first = 0; first < agents; first += chunk) {
      const uint64_t n = agents - first < chunk ? agents - first : chunk;
      block.Resize(n);
      if (!ReadBlock(uid, H5T_NATIVE_UINT64, first, n, 1, block.uid.data(),
                     why) ||
          !ReadBlock(position, H5T_NATIVE_DOUBLE, first, n, 3,
                     block.position.data(), why) ||
          !ReadBlock(diameter, H5T_NATIVE_DOUBLE, first, n, 1,
                     block.diameter.data(), why) ||
          !ReadBlock(cell_type, H5T_NATIVE_INT32, first, n, 1,
                     block.cell_type.data(), why) ||
          !ReadBlock(mother, H5T_NATIVE_UINT64, first, n, 1,
                     block.mother.data(), why)) {
        return false;
      }
      out->mismatches += block.Mismatches(first);
    }

    Id sim(H5Gopen2(file, "/records/abmio.Simulation", H5P_DEFAULT), H5Gclose);
    std::vector<uint64_t> step;
    std::vector<double> time;
    std::vector<std::string> unit;
    if (!sim.ok() || !ReadNumbers(sim, "step", H5T_NATIVE_UINT64, &step, why) ||
        !ReadNumbers(sim, "time", H5T_NATIVE_DOUBLE, &time, why) ||
        !ReadStrings(sim, "time_unit", &unit, why) || step.size() != 1 ||
        time.size() != 1 || unit.size() != 1) {
      ABMIO_FAIL(why, "bad /records/abmio.Simulation");
    }
    out->records.simulation.step = step[0];
    out->records.simulation.time = time[0];
    out->records.simulation.time_unit = unit[0];

    Id components(H5Gopen2(file, "/components", H5P_DEFAULT), H5Gclose);
    std::vector<std::string> name, version, role, digest, reference;
    std::vector<int8_t> affects;
    Id bool_type = BoolType();
    if (!components.ok() || !ReadStrings(components, "name", &name, why) ||
        !ReadStrings(components, "version", &version, why) ||
        !ReadStrings(components, "role", &role, why) ||
        !ReadStrings(components, "digest", &digest, why) ||
        !ReadNumbers(components, "affects_results", bool_type, &affects, why) ||
        !ReadStrings(components, "reference", &reference, why)) {
      return false;
    }
    out->records.components.clear();
    for (std::size_t i = 0; i < name.size(); ++i) {
      ComponentRecord c;
      c.name = name[i];
      c.version = version[i];
      c.role = role[i];
      c.digest = digest[i];
      c.affects_results = affects[i] != 0;
      c.reference = reference[i];
      out->records.components.push_back(c);
    }

    Id random(H5Gopen2(file, "/random", H5P_DEFAULT), H5Gclose);
    std::vector<std::string> owner;
    std::vector<uint64_t> key, counter;
    if (!random.ok() || !ReadStrings(random, "owner", &owner, why) ||
        !ReadNumbers(random, "key", H5T_NATIVE_UINT64, &key, why) ||
        !ReadNumbers(random, "counter", H5T_NATIVE_UINT64, &counter, why)) {
      return false;
    }
    out->records.streams.clear();
    for (std::size_t i = 0; i < owner.size(); ++i) {
      RandomStreamRecord r;
      r.owner = owner[i];
      r.key = key[i];
      r.counter = counter[i];
      out->records.streams.push_back(r);
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<Binding> CApiBinding() { return std::make_unique<CApi>(); }

}  // namespace h5binding
}  // namespace abmio
