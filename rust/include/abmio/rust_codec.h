// SPDX-License-Identifier: Apache-2.0
// The C interface of abm-io's Rust delta decoder (rust/, milestone R of
// docs/ROADMAP.md). Built and linked only with -DABMIO_RUST_CODEC=ON; the
// C++ decoder of abmio/codec.h is the one that ships.
#ifndef ABMIO_RUST_CODEC_H_
#define ABMIO_RUST_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decodes one frame into a caller-owned buffer, as abmio::DecodeDelta does.
//
// Returns a value of abmio::Status. On success *out_size holds the decoded
// size and that many bytes have been written to out; if the buffer was too
// small the status is kSizeMismatch and *out_size is the size needed.
//
// frame must point to frame_size readable bytes; previous to previous_size
// readable bytes, or be NULL; out to out_capacity writable bytes, or be NULL
// when out_capacity is zero.
uint8_t abmio_rust_decode_delta(const uint8_t* frame, size_t frame_size,
                                const uint8_t* previous, size_t previous_size,
                                uint8_t* out, size_t out_capacity,
                                size_t* out_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ABMIO_RUST_CODEC_H_
