// SPDX-License-Identifier: Apache-2.0
// The LZ4 filter for HDF5 (docs/INTEGRATION.md, "Compression"), provided by
// abm-io itself with H5Zregister and the LZ4 it already links, so checkpoints
// are written and read through HDF5's ordinary paths without a plugin.
//
// The bytes are the registered filter's (identifier 32004), so a third-party
// reader with the standard plugin, such as h5py with hdf5plugin, decodes them:
// the total decoded size as a big-endian 64-bit integer, the block size as a
// big-endian 32-bit integer, then each block as its big-endian 32-bit
// compressed size followed by its bytes. A block whose compressed size equals
// its decoded size is stored as it was, which is what the plugin does when
// LZ4 would not make a block smaller.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "abmio/status.h"

namespace abmio {

/// The filter identifier registered with The HDF Group for LZ4.
inline constexpr unsigned kLz4FilterId = 32004;

/// Registers the filter with the HDF5 library of this process. Idempotent;
/// returns false if HDF5 refused, which it reports through its own error
/// stack. Call it once before creating or opening a dataset that uses the
/// filter; a dataset is declared with H5Pset_filter(dcpl, kLz4FilterId, ...).
bool RegisterLz4Filter();

/// Whether HDF5 has the filter, from this registration or from a plugin.
bool Lz4FilterAvailable();

/// The default block size when a dataset's filter parameters give none,
/// the plugin's value.
inline constexpr uint32_t kLz4DefaultBlock = 1u << 30;

/// The filter's two halves, exposed so they can be tested and fuzzed without
/// HDF5. Pack encodes `n` bytes in blocks of `block` (0 means the default);
/// Unpack validates every header and size before touching `out`, returning
/// kTruncated when the input ends early, kBadHeader when the sizes do not add
/// up, and kCorrupt when LZ4 rejects a block.
void Lz4Pack(const uint8_t* in, std::size_t n, uint32_t block,
             std::vector<uint8_t>* out);
Status Lz4Unpack(const uint8_t* in, std::size_t n, std::vector<uint8_t>* out);

}  // namespace abmio
