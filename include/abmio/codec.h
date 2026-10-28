// SPDX-License-Identifier: Apache-2.0
// Optional link codec for slow networks: XOR with the previous message of the
// same layout, 8-byte shuffle, LZ4. Turn it off on fast fabrics, where it never
// pays.
#pragma once
#include <cstdint>
#include <vector>
#include "abmio/status.h"

namespace abmio {

// `prev` may be nullptr; otherwise it must hold `n` bytes.
std::vector<char> EncodeDelta(const char* cur, uint64_t n, const char* prev,
                              bool shuffle);

// Validates the frame. If the frame was encoded against a previous message,
// `prev` must hold exactly the decoded size.
Status DecodeDelta(const char* frame, uint64_t frame_size, const char* prev,
                   uint64_t prev_size, std::vector<char>& out);

}  // namespace abmio
