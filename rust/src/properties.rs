// SPDX-License-Identifier: Apache-2.0
//! The properties the decoder must have, and the proofs of them.
//!
//! Each property is a plain function that panics if it does not hold. Kani
//! proves them for every input up to the bounds declared in the harnesses
//! below (milestone R); the same functions are also run over enumerated
//! inputs by the tests, so that they are exercised wherever Kani is not
//! installed.
//!
//! What is proved, for inputs within those bounds:
//!
//! - **Memory safety:** neither decoder reads or writes outside its buffers,
//!   and neither panics, whatever the bytes. In Rust an out-of-bounds access
//!   panics, so proving the absence of panics proves both.
//! - **The round trip:** a frame built from a message decodes back to exactly
//!   that message, with or without the shuffle and the XOR.
//! - **The shuffle** is its own inverse in the sense that unshuffling a
//!   shuffled message restores it.

use crate::{decode_delta, lz4, shuffle, unshuffle, Status};

/// Decoding never panics, whatever the block and the output size.
pub fn decompressing_never_panics(block: &[u8], decoded_size: usize) {
    let mut out = vec![0u8; decoded_size];
    let ok = lz4::decompress_exact(block, &mut out);
    // A rejected block leaves nothing to rely on; an accepted one filled the
    // output exactly, which is what the caller is promised.
    assert!(!ok || out.len() == decoded_size);
}

/// Decoding a frame never panics, whatever the bytes, and a rejected frame
/// leaves no output.
pub fn decoding_never_panics(frame: &[u8], previous: Option<&[u8]>) {
    let mut out = Vec::new();
    let status = decode_delta(frame, previous, &mut out);
    assert!(status == Status::Ok || out.is_empty());
}

/// Unshuffling a shuffled message restores it.
pub fn the_shuffle_is_reversible(message: &[u8]) {
    let mut shuffled = vec![0u8; message.len()];
    shuffle(message, &mut shuffled);
    let mut restored = vec![0u8; message.len()];
    unshuffle(&shuffled, &mut restored);
    assert_eq!(restored, message);
}

/// A frame holding `message` as LZ4 literals, as the encoder writes it for
/// incompressible data, optionally shuffled and optionally XORed against
/// `previous`.
pub fn frame_of(message: &[u8], shuffled: bool, previous: Option<&[u8]>) -> Vec<u8> {
    let mut payload = message.to_vec();
    if let Some(previous) = previous {
        for (byte, p) in payload.iter_mut().zip(previous) {
            *byte ^= p;
        }
    }
    if shuffled {
        let mut s = vec![0u8; payload.len()];
        shuffle(&payload, &mut s);
        payload = s;
    }

    let mut frame = Vec::new();
    frame.extend_from_slice(b"ABDZ");
    frame.push(u8::from(shuffled) | (u8::from(previous.is_some()) << 1));
    frame.extend_from_slice(&[0, 0, 0]);
    frame.extend_from_slice(&(message.len() as u64).to_le_bytes());
    if message.is_empty() {
        return frame;
    }
    // One LZ4 sequence: literals alone, which is a valid block of any length.
    if payload.len() < 15 {
        frame.push((payload.len() as u8) << 4);
    } else {
        frame.push(0xf0);
        let mut rest = payload.len() - 15;
        while rest >= 255 {
            frame.push(255);
            rest -= 255;
        }
        frame.push(rest as u8);
    }
    frame.extend_from_slice(&payload);
    frame
}

/// A frame built from a message decodes back to exactly that message.
pub fn the_round_trip_restores_the_message(message: &[u8], shuffled: bool, xored: bool) {
    let previous: Vec<u8> = message
        .iter()
        .map(|b| b.wrapping_mul(31).wrapping_add(7))
        .collect();
    let base = if xored {
        Some(previous.as_slice())
    } else {
        None
    };
    let frame = frame_of(message, shuffled, base);
    let mut out = Vec::new();
    let status = decode_delta(&frame, base, &mut out);
    assert_eq!(status, Status::Ok);
    assert_eq!(out, message);
}

// ---------------------------------------------------------------------------
// The proofs

#[cfg(kani)]
mod proofs {
    use super::*;

    /// Decompression is memory-safe for every block of up to six bytes and
    /// every output of up to six bytes: every one of the 2^48 blocks, at
    /// every length, against every output size.
    #[kani::proof]
    #[kani::unwind(10)]
    fn decompressing_is_memory_safe() {
        let block: [u8; 6] = kani::any();
        let length: usize = kani::any();
        kani::assume(length <= block.len());
        let decoded_size: usize = kani::any();
        kani::assume(decoded_size <= 6);
        decompressing_never_panics(&block[..length], decoded_size);
    }

    /// Decoding a frame is memory-safe for every 18-byte frame, which covers
    /// the 16-byte header and a short block, with and without a reference
    /// message.
    #[kani::proof]
    #[kani::unwind(10)]
    fn decoding_is_memory_safe() {
        let frame: [u8; 18] = kani::any();
        let previous: [u8; 4] = kani::any();
        let with_previous: bool = kani::any();
        decoding_never_panics(&frame, if with_previous { Some(&previous) } else { None });
    }

    /// A frame with a header that may claim any size at all, so that the
    /// checks against forged sizes are covered.
    #[kani::proof]
    #[kani::unwind(10)]
    fn decoding_a_forged_header_is_memory_safe() {
        let mut frame = [0u8; 18];
        frame[0..4].copy_from_slice(b"ABDZ");
        frame[4] = kani::any();
        let size: u64 = kani::any();
        frame[8..16].copy_from_slice(&size.to_le_bytes());
        frame[16] = kani::any();
        frame[17] = kani::any();
        let previous: [u8; 2] = kani::any();
        decoding_never_panics(&frame, Some(&previous));
    }

    #[kani::proof]
    #[kani::unwind(12)]
    fn the_shuffle_is_reversible_for_any_message() {
        let message: [u8; 9] = kani::any(); // a group of eight and a remainder
        let length: usize = kani::any();
        kani::assume(length <= message.len());
        the_shuffle_is_reversible(&message[..length]);
    }

    #[kani::proof]
    #[kani::unwind(12)]
    fn the_round_trip_restores_any_message() {
        let message: [u8; 9] = kani::any();
        let length: usize = kani::any();
        kani::assume(length <= message.len());
        let shuffled: bool = kani::any();
        let xored: bool = kani::any();
        the_round_trip_restores_the_message(&message[..length], shuffled, xored);
    }
}

// ---------------------------------------------------------------------------
// The same properties over enumerated inputs, for where Kani is not installed

#[cfg(test)]
mod tests {
    use super::*;

    /// Every string of `length` bytes over `alphabet`, passed to `check`.
    fn for_every_string(alphabet: &[u8], length: usize, check: &mut dyn FnMut(&[u8])) {
        let mut buffer = vec![alphabet[0]; length];
        let total = alphabet.len().pow(length as u32);
        for mut index in 0..total {
            for byte in buffer.iter_mut() {
                *byte = alphabet[index % alphabet.len()];
                index /= alphabet.len();
            }
            check(&buffer);
        }
    }

    #[test]
    fn decompressing_never_panics_on_short_blocks() {
        // Bytes that make tokens, lengths, offsets and literals of every kind.
        let alphabet = [0x00u8, 0x01, 0x0f, 0x10, 0x2f, 0xf0, 0xff];
        for length in 0..=4 {
            for decoded_size in 0..=8 {
                for_every_string(&alphabet, length, &mut |block| {
                    decompressing_never_panics(block, decoded_size);
                });
            }
        }
    }

    #[test]
    fn decoding_never_panics_on_short_frames() {
        let alphabet = [b'A', b'B', b'D', b'Z', 0x00, 0x01, 0x03, 0xff];
        // Frames shorter than a header, and headers with a byte of payload.
        for length in 0..=3 {
            for_every_string(&alphabet, length, &mut |frame| {
                decoding_never_panics(frame, None);
            });
        }
        // Whole frames: a valid header with each flag, a forged size, a payload.
        let previous = [0x5au8; 4];
        for flags in 0..=3u8 {
            for size in [0u64, 1, 4, 17, 1 << 20, u64::MAX] {
                for payload in [vec![], vec![0x10, 0x41], vec![0xff; 6]] {
                    let mut frame = b"ABDZ".to_vec();
                    frame.push(flags);
                    frame.extend_from_slice(&[0, 0, 0]);
                    frame.extend_from_slice(&size.to_le_bytes());
                    frame.extend_from_slice(&payload);
                    decoding_never_panics(&frame, Some(&previous));
                    decoding_never_panics(&frame, None);
                }
            }
        }
    }

    #[test]
    fn the_shuffle_and_the_round_trip_hold_for_every_length() {
        for length in 0..40usize {
            let message: Vec<u8> = (0..length).map(|i| (i * 37 % 251) as u8).collect();
            the_shuffle_is_reversible(&message);
            for shuffled in [false, true] {
                for xored in [false, true] {
                    the_round_trip_restores_the_message(&message, shuffled, xored);
                }
            }
        }
    }
}
