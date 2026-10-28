// SPDX-License-Identifier: Apache-2.0
// Default exchange path (design A): agents packed per type into blocks of
// rows or columns through generated field visitors, unpacked into objects the
// receiver owns and reuses across messages.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "abmio/generated.h"
#include "abmio/status.h"

namespace abmio {

enum class Layout : uint8_t { kRows = 1, kColumns = 2 };

struct Arena {
  std::vector<std::unique_ptr<Agent>> pool[gen::kNumAgentTypes + 1];
  std::vector<Agent*>
      view;  // agents of the last message, grouped by type within each block
};

// Packs `agents` into `out` (resized as needed; may stay larger than the
// result). Returns the message size in bytes. Throws std::invalid_argument for
// unregistered types, more than 255 behaviors per agent or a bad block size.
uint64_t Pack(const std::vector<Agent*>& agents, Layout layout,
              std::vector<char>& out, uint32_t block = 1024);

// Validates and unpacks a message of exactly `size` bytes. Never reads outside
// [data, data + size). On error, `arena.view` is cleared and pooled objects
// stay valid.
Status Unpack(const char* data, uint64_t size, Arena& arena);

}  // namespace abmio
