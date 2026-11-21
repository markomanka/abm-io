// SPDX-License-Identifier: Apache-2.0
// The manifest and pointer parsers on arbitrary bytes: they must refuse or
// accept without reading out of bounds, and whatever they accept must come
// back the same through the writer and the parser again.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "abmio/manifest.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const char* text = reinterpret_cast<const char*>(data);
  abmio::Manifest m;
  if (abmio::ParseManifest(text, size, &m, nullptr) == abmio::Status::kOk) {
    const std::string again = abmio::ToJson(m);
    abmio::Manifest back;
    if (abmio::ParseManifest(again.data(), again.size(), &back, nullptr) !=
            abmio::Status::kOk ||
        !(back == m)) {
      std::abort();
    }
  }
  std::string name;
  if (abmio::ParsePointer(text, size, &name, nullptr) == abmio::Status::kOk) {
    const std::string again = abmio::PointerJson(name);
    std::string back;
    if (abmio::ParsePointer(again.data(), again.size(), &back, nullptr) !=
            abmio::Status::kOk ||
        back != name) {
      std::abort();
    }
  }
  return 0;
}
