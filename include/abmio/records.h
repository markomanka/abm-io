// SPDX-License-Identifier: Apache-2.0
// The types every checkpoint carries beside its agents: the simulation's own
// state, the random streams, and what the run depended on
// (docs/INTEGRATION.md, "Tables and records").
//
// They are described like any other type, so they version, evolve and are
// checked on a strict resume by the same rules. A record is a table with one
// row; nothing here needs a mechanism of its own.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "abmio/schema.h"

namespace abmio {

/// Where the simulation had got to. One row.
struct SimulationRecord {
  uint64_t step = 0;      ///< completed steps
  double time = 0;        ///< simulated time, in `time_unit`
  std::string time_unit;  ///< "min", "h", "s"; recorded, never converted
};

/// One random stream. One row per stream, so a run with a stream per thread,
/// or later per agent, is the same table with more rows.
struct RandomStreamRecord {
  std::string owner;  ///< "thread 3", "agent", whatever the simulator calls it
  uint64_t key = 0;   ///< the counter-based generator's key
  uint64_t counter = 0;  ///< how far along the stream is
};

/// What wrote this checkpoint. One row.
struct ProvenanceRecord {
  std::string abmio_version;
  std::string schema_digest;  ///< of `/schema`, so a reader can tell it apart
  uint64_t seed = 0;          ///< the run's seed
  std::string compression;    ///< the decision: "off", "lz4"
  std::string
      compression_inputs;  ///< the declared inputs it was made from, as JSON
  std::string created;     ///< ISO 8601, UTC
};

/// One thing the run depended on. The row has the same shape whatever the
/// dependency is, so a kind nobody has thought of yet needs no new field:
/// a solver, a model file, a learned operation's weights, an instrument.
struct ComponentRecord {
  std::string name;
  std::string version;
  std::string
      role;  ///< simulator, solver, library, model, surrogate, instrument, ...
  std::string digest;  ///< content hash where one can be had, else empty
  bool affects_results = true;
  std::string reference;  ///< a DOI, a URL, a commit; optional
};

/// One accepted discontinuity: a resume that went ahead although a component
/// affecting results had changed. A run that carries these rows is not a
/// continuation of the original, and says so; see docs/INTEGRATION.md.
struct LineageRecord {
  uint64_t step = 0;      ///< where the run was resumed
  std::string component;  ///< which one changed
  std::string from_version;
  std::string to_version;
  std::string accepted_by;  ///< who or what accepted it
  std::string reason;       ///< why, in the accepter's words
  std::string at;           ///< ISO 8601, UTC
};

/// Every difference that affects results between the components a checkpoint
/// was written with and those present now: a changed version, the same version
/// with different content, a component gone or a new one. A component counts
/// as affecting results if it is declared to on either side, so changing the
/// declaration does not hide a change of version. Reporting only: the decision
/// belongs to the caller.
bool ComponentsDiffer(const std::vector<ComponentRecord>& before,
                      const std::vector<ComponentRecord>& now,
                      std::vector<std::string>* differences);

/// The differences that stop a resume: those that affect results and whose
/// component is not named in `accepted`. Names are compared exactly; a prefix
/// accepts nothing. Acceptance is per component on purpose, so that going
/// ahead means naming what changed rather than setting a flag that means
/// everything. A caller that accepts a difference records a
/// LineageRecord for it, so the discontinuity travels with the result.
bool BlockingDifferences(const std::vector<ComponentRecord>& before,
                         const std::vector<ComponentRecord>& now,
                         const std::vector<std::string>& accepted,
                         std::vector<std::string>* blocking);

template <>
struct TypeTraits<LineageRecord> {
  static constexpr const char* kName = "abmio.Lineage";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("step"), &LineageRecord::step);
    v(Field("component"), &LineageRecord::component);
    v(Field("from_version"), &LineageRecord::from_version);
    v(Field("to_version"), &LineageRecord::to_version);
    v(Field("accepted_by"), &LineageRecord::accepted_by);
    v(Field("reason"), &LineageRecord::reason);
    v(Field("at"), &LineageRecord::at);
  }
};

template <>
struct TypeTraits<SimulationRecord> {
  static constexpr const char* kName = "abmio.Simulation";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("step"), &SimulationRecord::step);
    v(Field("time"), &SimulationRecord::time);
    v(Field("time_unit"), &SimulationRecord::time_unit);
  }
};

template <>
struct TypeTraits<RandomStreamRecord> {
  static constexpr const char* kName = "abmio.RandomStream";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("owner"), &RandomStreamRecord::owner);
    v(Field("key"), &RandomStreamRecord::key);
    v(Field("counter"), &RandomStreamRecord::counter);
  }
};

template <>
struct TypeTraits<ProvenanceRecord> {
  static constexpr const char* kName = "abmio.Provenance";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("abmio_version"), &ProvenanceRecord::abmio_version);
    v(Field("schema_digest"), &ProvenanceRecord::schema_digest);
    v(Field("seed"), &ProvenanceRecord::seed);
    v(Field("compression"), &ProvenanceRecord::compression);
    v(Field("compression_inputs"), &ProvenanceRecord::compression_inputs);
    v(Field("created"), &ProvenanceRecord::created);
  }
};

template <>
struct TypeTraits<ComponentRecord> {
  static constexpr const char* kName = "abmio.Component";
  static constexpr uint32_t kVersion = 1;
  using Base = void;
  template <class V>
  static void Visit(V& v) {
    v(Field("name"), &ComponentRecord::name);
    v(Field("version"), &ComponentRecord::version);
    v(Field("role"), &ComponentRecord::role);
    v(Field("digest"), &ComponentRecord::digest);
    v(Field("affects_results"), &ComponentRecord::affects_results);
    v(Field("reference"), &ComponentRecord::reference);
  }
};

}  // namespace abmio
