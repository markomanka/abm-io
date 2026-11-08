// SPDX-License-Identifier: Apache-2.0
// The checkpoint writer and reader of bench/h5binding/binding.h on HighFive.
// Everything goes through HighFive except what it has no property for, which
// is marked "escape hatch" where it happens, so the count in docs/FINDINGS.md
// can be checked against the source.
#include <hdf5.h>
#include <highfive/highfive.hpp>

#include <memory>
#include <string>
#include <vector>

#include "abmio/h5lz4.h"
#include "binding.h"

namespace abmio {
namespace h5binding {
namespace {

namespace hf = HighFive;

// Escape hatch 1: HighFive has properties for gzip, szip and shuffle but none
// for a filter by identifier, so the LZ4 filter is a property of our own,
// which HighFive accepts because `add` takes anything with apply(hid_t).
// The call inside is the C API's.
class Lz4Filter {
 public:
  void apply(hid_t dcpl) const {
    if (H5Pset_filter(dcpl, kLz4FilterId, H5Z_FLAG_MANDATORY, 0, nullptr) < 0) {
      throw hf::PropertyException("cannot set the LZ4 filter");
    }
  }
};

// Escape hatch 2: HighFive has AllocationTime but no property for the fill
// time, and without filling on allocation the unwritten part of a partial
// last chunk holds whatever was in memory, so the same agents give different
// bytes on each run (found by `cmp` in the comparison). Same mechanism as the
// filter: a property of our own around the C call.
class FillOnAllocation {
 public:
  void apply(hid_t dcpl) const {
    if (H5Pset_fill_time(dcpl, H5D_FILL_TIME_ALLOC) < 0) {
      throw hf::PropertyException("cannot set the fill time");
    }
  }
};

hf::DataSetCreateProps AgentCreateProps(const Layout& layout, hsize_t width) {
  hf::DataSetCreateProps dcpl;
  if (width > 1) {
    dcpl.add(hf::Chunking({layout.chunk, width}));
  } else {
    dcpl.add(hf::Chunking({layout.chunk}));
  }
  dcpl.add(FillOnAllocation());
  if (layout.lz4) {
    dcpl.add(Lz4Filter());
  }
  return dcpl;
}

hf::DataSetAccessProps AgentAccessProps(const Layout& layout) {
  hf::DataSetAccessProps dapl;
  if (layout.cache_bytes > 0) {
    dapl.add(hf::Caching(521, layout.cache_bytes, 1.0));
  }
  return dapl;
}

template <class T>
hf::DataSet CreateAgentColumn(hf::Group& group, const std::string& name,
                              std::size_t width, const Layout& layout) {
  const hf::DataSpace space =
      width > 1 ? hf::DataSpace({0, width}, {hf::DataSpace::UNLIMITED, width})
                : hf::DataSpace({0}, {hf::DataSpace::UNLIMITED});
  return group.createDataSet<T>(name, space, AgentCreateProps(layout, width),
                                AgentAccessProps(layout));
}

template <class T>
void Append(hf::DataSet& set, std::size_t first, std::size_t count,
            std::size_t width, const T* data) {
  if (width > 1) {
    set.resize({first + count, width});
    set.select({first, 0}, {count, width}).write_raw(data);
  } else {
    set.resize({first + count});
    set.select({first}, {count}).write_raw(data);
  }
}

template <class T>
void ReadBlock(const hf::DataSet& set, std::size_t first, std::size_t count,
               std::size_t width, T* data) {
  if (width > 1) {
    set.select({first, 0}, {count, width}).read_raw(data);
  } else {
    set.select({first}, {count}).read_raw(data);
  }
}

// Escape hatch 3, on reading only: HighFive reports gzip and shuffle as
// properties, not an arbitrary filter's identifier, so the declared filters
// are listed through the C API on the property list HighFive hands out.
std::vector<unsigned> Filters(const hf::DataSet& set) {
  std::vector<unsigned> out;
  const hf::DataSetCreateProps dcpl = set.getCreatePropertyList();
  const int n = H5Pget_nfilters(dcpl.getId());
  for (int i = 0; i < n; ++i) {
    unsigned flags = 0;
    size_t nelmts = 0;
    out.push_back(static_cast<unsigned>(
        H5Pget_filter2(dcpl.getId(), static_cast<unsigned>(i), &flags, &nelmts,
                       nullptr, 0, nullptr, nullptr)));
  }
  return out;
}

class HighFiveBindingImpl : public Binding {
 public:
  const char* Name() const override { return "highfive"; }

  bool Write(const std::string& path, const Layout& layout,
             const Records& records, std::string* why) override {
    if (layout.lz4 && !RegisterLz4Filter()) {
      *why = "cannot register the LZ4 filter";
      return false;
    }
    try {
      hf::File file(path, hf::File::Truncate);
      file.createAttribute<uint32_t>("format_version", 1u);
      file.createAttribute<std::string>("producer",
                                        std::string("abm-io h5binding"));
      file.createDataSet<std::string>("schema", std::string("{\"types\": []}"));

      hf::Group cell = file.createGroup("/agents/cell");
      hf::DataSet uid = CreateAgentColumn<uint64_t>(cell, "uid", 1, layout);
      hf::DataSet position =
          CreateAgentColumn<double>(cell, "position", 3, layout);
      hf::DataSet diameter =
          CreateAgentColumn<double>(cell, "diameter", 1, layout);
      hf::DataSet cell_type =
          CreateAgentColumn<int32_t>(cell, "cell_type", 1, layout);
      hf::DataSet mother =
          CreateAgentColumn<uint64_t>(cell, "mother", 1, layout);
      Block block;
      for (uint64_t first = 0; first < layout.agents; first += layout.chunk) {
        const uint64_t n = layout.agents - first < layout.chunk
                               ? layout.agents - first
                               : layout.chunk;
        block.Fill(first, n);
        Append(uid, first, n, 1, block.uid.data());
        Append(position, first, n, 3, block.position.data());
        Append(diameter, first, n, 1, block.diameter.data());
        Append(cell_type, first, n, 1, block.cell_type.data());
        Append(mother, first, n, 1, block.mother.data());
      }

      hf::Group sim = file.createGroup("/records/abmio.Simulation");
      sim.createDataSet("step", std::vector<uint64_t>{records.simulation.step});
      sim.createDataSet("time", std::vector<double>{records.simulation.time});
      sim.createDataSet("time_unit",
                        std::vector<std::string>{records.simulation.time_unit});

      hf::Group components = file.createGroup("/components");
      std::vector<std::string> name, version, role, digest, reference;
      std::vector<bool> affects;
      for (const ComponentRecord& c : records.components) {
        name.push_back(c.name);
        version.push_back(c.version);
        role.push_back(c.role);
        digest.push_back(c.digest);
        reference.push_back(c.reference);
        affects.push_back(c.affects_results);
      }
      components.createDataSet("name", name);
      components.createDataSet("version", version);
      components.createDataSet("role", role);
      components.createDataSet("digest", digest);
      components.createDataSet("affects_results", affects);
      components.createDataSet("reference", reference);

      hf::Group random = file.createGroup("/random");
      std::vector<std::string> owner;
      std::vector<uint64_t> key, counter;
      for (const RandomStreamRecord& r : records.streams) {
        owner.push_back(r.owner);
        key.push_back(r.key);
        counter.push_back(r.counter);
      }
      random.createDataSet("owner", owner);
      random.createDataSet("key", key);
      random.createDataSet("counter", counter);
    } catch (const hf::Exception& e) {
      *why = e.what();
      return false;
    }
    return true;
  }

  bool Read(const std::string& path, uint64_t chunk, ReadResult* out,
            std::string* why) override {
    RegisterLz4Filter();
    try {
      hf::File file(path, hf::File::ReadOnly);
      hf::Group cell = file.getGroup("/agents/cell");
      hf::DataSet uid = cell.getDataSet("uid");
      hf::DataSet position = cell.getDataSet("position");
      hf::DataSet diameter = cell.getDataSet("diameter");
      hf::DataSet cell_type = cell.getDataSet("cell_type");
      hf::DataSet mother = cell.getDataSet("mother");
      out->filters = Filters(position);
      const uint64_t agents = uid.getDimensions()[0];
      out->agents = agents;
      out->mismatches = 0;
      Block block;
      for (uint64_t first = 0; first < agents; first += chunk) {
        const uint64_t n = agents - first < chunk ? agents - first : chunk;
        block.Resize(n);
        ReadBlock(uid, first, n, 1, block.uid.data());
        ReadBlock(position, first, n, 3, block.position.data());
        ReadBlock(diameter, first, n, 1, block.diameter.data());
        ReadBlock(cell_type, first, n, 1, block.cell_type.data());
        ReadBlock(mother, first, n, 1, block.mother.data());
        out->mismatches += block.Mismatches(first);
      }

      hf::Group sim = file.getGroup("/records/abmio.Simulation");
      std::vector<uint64_t> step;
      std::vector<double> time;
      std::vector<std::string> unit;
      sim.getDataSet("step").read(step);
      sim.getDataSet("time").read(time);
      sim.getDataSet("time_unit").read(unit);
      if (step.size() != 1 || time.size() != 1 || unit.size() != 1) {
        *why = "bad /records/abmio.Simulation";
        return false;
      }
      out->records.simulation.step = step[0];
      out->records.simulation.time = time[0];
      out->records.simulation.time_unit = unit[0];

      hf::Group components = file.getGroup("/components");
      std::vector<std::string> name, version, role, digest, reference;
      std::vector<bool> affects;
      components.getDataSet("name").read(name);
      components.getDataSet("version").read(version);
      components.getDataSet("role").read(role);
      components.getDataSet("digest").read(digest);
      components.getDataSet("affects_results").read(affects);
      components.getDataSet("reference").read(reference);
      out->records.components.clear();
      for (std::size_t i = 0; i < name.size(); ++i) {
        ComponentRecord c;
        c.name = name[i];
        c.version = version[i];
        c.role = role[i];
        c.digest = digest[i];
        c.affects_results = affects[i];
        c.reference = reference[i];
        out->records.components.push_back(c);
      }

      hf::Group random = file.getGroup("/random");
      std::vector<std::string> owner;
      std::vector<uint64_t> key, counter;
      random.getDataSet("owner").read(owner);
      random.getDataSet("key").read(key);
      random.getDataSet("counter").read(counter);
      out->records.streams.clear();
      for (std::size_t i = 0; i < owner.size(); ++i) {
        RandomStreamRecord r;
        r.owner = owner[i];
        r.key = key[i];
        r.counter = counter[i];
        out->records.streams.push_back(r);
      }
    } catch (const hf::Exception& e) {
      *why = e.what();
      return false;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<Binding> HighFiveBinding() {
  return std::make_unique<HighFiveBindingImpl>();
}

}  // namespace h5binding
}  // namespace abmio
