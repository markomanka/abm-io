// SPDX-License-Identifier: Apache-2.0
// A directory of checkpoints in which a checkpoint becomes visible only when
// it is complete (docs/INTEGRATION.md; docs/RATIONALE.md, entry 10):
//
//   <root>/latest                     the pointer: names the newest checkpoint
//   <root>/<checkpoint>/part-00000.h5 one file per writer, each written as
//                                     part-00000.h5.tmp and renamed when closed
//   <root>/<checkpoint>/manifest.json lists the parts and their sizes
//
// Each writer writes its part with CheckpointWriter, which renames the file to
// its final name in Close. When every writer has finished, one process calls
// CommitCheckpoint, which writes the manifest and then replaces the pointer,
// each by rename after flushing to disk. FindLatest follows the pointer and
// checks the manifest against the parts, so a reader sees the previous
// complete checkpoint or the new one, never a mixture, wherever a job was
// killed. Nothing is deleted: which checkpoints to keep is the caller's.
//
// The guarantee rests on rename being atomic, which POSIX file systems
// provide and most object stores do not (docs/STORAGE-BACKENDS.md).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "abmio/manifest.h"
#include "abmio/status.h"

namespace abmio {

/// The file name of part `part`: "part-00000.h5", with more digits as needed.
std::string PartName(uint32_t part);

/// Creates <root>/<checkpoint> and sets `directory` to it, where the parts are
/// then written. A checkpoint that already has a manifest is not reopened.
/// `why` receives a sentence on failure, here and below.
Status PrepareCheckpoint(const std::string& root, const std::string& checkpoint,
                         std::string* directory, std::string* why);

/// Makes the checkpoint visible: lists parts 0 to parts - 1 with their sizes
/// in its manifest, then points <root>/latest at it. Refused, with nothing
/// changed, if a part is missing.
Status CommitCheckpoint(const std::string& root, const std::string& checkpoint,
                        uint32_t parts, std::string* why);

struct FoundCheckpoint {
  bool found = false;     ///< false: the directory holds no checkpoint yet
  std::string directory;  ///< <root>/<checkpoint>
  Manifest manifest;      ///< as read and checked
  std::vector<std::string> part_paths;  ///< in the manifest's order
};

/// Follows the pointer to the newest complete checkpoint and checks that its
/// manifest names it and that every part listed is there with the size
/// listed. A directory with no pointer gives kOk with found false.
Status FindLatest(const std::string& root, FoundCheckpoint* out,
                  std::string* why);

}  // namespace abmio
