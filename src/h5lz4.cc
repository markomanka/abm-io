// SPDX-License-Identifier: Apache-2.0
#include "abmio/h5lz4.h"

#include <hdf5.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace abmio {
namespace {

// The filter callback HDF5 invokes for every chunk, in both directions. It
// takes the chunk in *buf and leaves the result there, in a buffer HDF5 frees
// with free(), which is why the result is copied out of a vector rather than
// kept in one. Returns 0 on failure, as the filter protocol requires.
size_t FilterCallback(unsigned flags, size_t cd_nelmts,
                      const unsigned cd_values[], size_t nbytes,
                      size_t* buf_size, void** buf) {
  const uint8_t* in = static_cast<const uint8_t*>(*buf);
  std::vector<uint8_t> out;
  try {
    if (flags & H5Z_FLAG_REVERSE) {
      if (Lz4Unpack(in, nbytes, &out) != Status::kOk) {
        return 0;
      }
    } else {
      const uint32_t block =
          cd_nelmts > 0 && cd_values[0] > 0 ? cd_values[0] : 0;
      Lz4Pack(in, nbytes, block, &out);
    }
  } catch (const std::bad_alloc&) {
    return 0;  // an exception must not cross into HDF5's C frames
  }
  void* result = std::malloc(out.empty() ? 1 : out.size());
  if (result == nullptr) {
    return 0;
  }
  std::memcpy(result, out.data(), out.size());
  std::free(*buf);
  *buf = result;
  *buf_size = out.size();
  return out.size();
}

const H5Z_class2_t kFilterClass = {
    H5Z_CLASS_T_VERS,
    static_cast<H5Z_filter_t>(kLz4FilterId),
    1,  // encoder present
    1,  // decoder present
    "abm-io LZ4 (HDF5 filter 32004)",
    nullptr,  // can_apply: any dataset
    nullptr,  // set_local: nothing to derive per dataset
    FilterCallback,
};

}  // namespace

bool RegisterLz4Filter() {
  if (Lz4FilterAvailable()) {
    return true;
  }
  return H5Zregister(&kFilterClass) >= 0;
}

bool Lz4FilterAvailable() {
  return H5Zfilter_avail(static_cast<H5Z_filter_t>(kLz4FilterId)) > 0;
}

}  // namespace abmio
