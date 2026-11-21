// SPDX-License-Identifier: Apache-2.0
// The two small files that make a checkpoint visible only when it is complete
// (docs/INTEGRATION.md, "A checkpoint becomes visible only when it is
// complete"; docs/RATIONALE.md, entry 10): the manifest, which lists a
// checkpoint's parts, and the pointer, which names the newest checkpoint.
//
// Both are JSON of a fixed shape, so that `cat` shows them and any language
// reads them. They are read from files that may be damaged or hostile, so
// the parsers accept that shape and nothing else: no unknown or repeated keys,
// no escapes, names only from IsCheckpointName's alphabet, numbers that fit in
// 64 bits, at most kMaxManifestBytes of text. Each carries a format version;
// a file of a newer version is refused with the version named.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "abmio/status.h"

namespace abmio {

constexpr uint32_t kManifestFormat = 1;
constexpr uint32_t kPointerFormat = 1;
constexpr std::size_t kMaxManifestBytes = std::size_t{16} << 20;

/// Whether `name` may name a checkpoint or a part: 1 to 128 characters from
/// letters, digits, '.', '_' and '-', not starting with '.', and neither
/// "latest" nor ending in ".tmp", which the store uses itself. Such a name
/// cannot leave the directory it is joined to.
bool IsCheckpointName(const std::string& name);

struct ManifestPart {
  std::string name;    ///< file name within the checkpoint's directory
  uint64_t bytes = 0;  ///< its size once complete
  bool operator==(const ManifestPart& o) const {
    return name == o.name && bytes == o.bytes;
  }
};

struct Manifest {
  std::string checkpoint;  ///< the checkpoint's name, its directory's name
  std::vector<ManifestPart> parts;
  bool operator==(const Manifest& o) const {
    return checkpoint == o.checkpoint && parts == o.parts;
  }
};

/// The manifest as JSON; the same manifest always gives the same text.
std::string ToJson(const Manifest& manifest);
/// Parses a manifest. `why` receives a sentence on failure, here and below.
Status ParseManifest(const char* data, std::size_t size, Manifest* out,
                     std::string* why);

/// The pointer's JSON, naming the newest complete checkpoint.
std::string PointerJson(const std::string& checkpoint);
Status ParsePointer(const char* data, std::size_t size, std::string* checkpoint,
                    std::string* why);

}  // namespace abmio
