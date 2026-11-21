// SPDX-License-Identifier: Apache-2.0
#include "abmio/store.h"

#include <cstdio>

#include "durable.h"

namespace abmio {
namespace {

Status Fail(Status s, std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message;
  }
  return s;
}

std::string Join(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  return directory.back() == '/' ? directory + name : directory + "/" + name;
}

const char* const kManifest = "manifest.json";
const char* const kPointer = "latest";

}  // namespace

std::string PartName(uint32_t part) {
  char digits[16];
  std::snprintf(digits, sizeof digits, "%05u", static_cast<unsigned>(part));
  return std::string("part-") + digits + ".h5";
}

Status PrepareCheckpoint(const std::string& root, const std::string& checkpoint,
                         std::string* directory, std::string* why) {
  if (!IsCheckpointName(checkpoint)) {
    return Fail(Status::kBadSchema, why,
                "\"" + checkpoint + "\" cannot name a checkpoint");
  }
  const std::string d = Join(root, checkpoint);
  Status s = durable::MakeDirectories(d, why);
  if (s != Status::kOk) {
    return s;
  }
  uint64_t bytes = 0;
  bool missing = false;
  s = durable::SizeOf(Join(d, kManifest), &bytes, &missing, why);
  if (s != Status::kOk) {
    return s;
  }
  if (!missing) {
    return Fail(Status::kBadSchema, why,
                "checkpoint " + checkpoint + " is already complete");
  }
  *directory = d;
  return Status::kOk;
}

Status CommitCheckpoint(const std::string& root, const std::string& checkpoint,
                        uint32_t parts, std::string* why) {
  if (!IsCheckpointName(checkpoint)) {
    return Fail(Status::kBadSchema, why,
                "\"" + checkpoint + "\" cannot name a checkpoint");
  }
  const std::string d = Join(root, checkpoint);
  Manifest manifest;
  manifest.checkpoint = checkpoint;
  for (uint32_t i = 0; i < parts; ++i) {
    ManifestPart part;
    part.name = PartName(i);
    bool missing = false;
    Status s = durable::SizeOf(Join(d, part.name), &part.bytes, &missing, why);
    if (s != Status::kOk) {
      return s;
    }
    if (missing) {
      return Fail(Status::kIo, why,
                  "checkpoint " + checkpoint + " is not complete: " +
                      part.name + " has not been closed");
    }
    manifest.parts.push_back(part);
  }
  Status s = durable::WriteFile(Join(d, kManifest), ToJson(manifest), why);
  if (s != Status::kOk) {
    return s;
  }
  return durable::WriteFile(Join(root, kPointer), PointerJson(checkpoint), why);
}

Status FindLatest(const std::string& root, FoundCheckpoint* out,
                  std::string* why) {
  *out = FoundCheckpoint();
  std::string text;
  bool missing = false;
  Status s = durable::ReadFile(Join(root, kPointer), kMaxManifestBytes, &text,
                               &missing, why);
  if (s != Status::kOk || missing) {
    return s;
  }
  std::string checkpoint;
  s = ParsePointer(text.data(), text.size(), &checkpoint, why);
  if (s != Status::kOk) {
    return s;
  }
  const std::string d = Join(root, checkpoint);
  s = durable::ReadFile(Join(d, kManifest), kMaxManifestBytes, &text, &missing,
                        why);
  if (s != Status::kOk) {
    return s;
  }
  if (missing) {
    return Fail(Status::kCorrupt, why,
                "the pointer names checkpoint " + checkpoint +
                    ", which has no manifest");
  }
  Manifest manifest;
  s = ParseManifest(text.data(), text.size(), &manifest, why);
  if (s != Status::kOk) {
    return s;
  }
  if (manifest.checkpoint != checkpoint) {
    return Fail(
        Status::kCorrupt, why,
        "the manifest of " + checkpoint + " names " + manifest.checkpoint);
  }
  std::vector<std::string> paths;
  for (const ManifestPart& part : manifest.parts) {
    const std::string path = Join(d, part.name);
    uint64_t bytes = 0;
    s = durable::SizeOf(path, &bytes, &missing, why);
    if (s != Status::kOk) {
      return s;
    }
    if (missing || bytes != part.bytes) {
      return Fail(Status::kCorrupt, why,
                  "checkpoint " + checkpoint + " lists " + part.name +
                      (missing ? ", which is missing"
                               : ", whose size differs from the manifest's"));
    }
    paths.push_back(path);
  }
  out->found = true;
  out->directory = d;
  out->manifest = std::move(manifest);
  out->part_paths = std::move(paths);
  return Status::kOk;
}

}  // namespace abmio
