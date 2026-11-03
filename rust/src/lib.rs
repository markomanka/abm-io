// SPDX-License-Identifier: Apache-2.0
//! abm-io's delta decoder in Rust, behind a C interface.
//!
//! An experiment (docs/ROADMAP.md, milestone R): the same frames as
//! `src/codec.cc`, decoded in safe Rust so that memory safety and the round
//! trip can be proved with Kani, then compared with the C++ decoder by
//! differential fuzzing and benchmarks. The C++ decoder remains the one that
//! ships; this one is built only when `ABMIO_RUST_CODEC=ON`.
//!
//! Frame (`src/codec.cc`): "ABDZ" | flags u8 (bit 0 shuffle, bit 1 XOR with
//! the previous message) | three zero bytes | decoded size u64 little-endian |
//! LZ4 block.

#![forbid(unsafe_op_in_unsafe_fn)]

pub mod ffi;
pub mod lz4;

/// Mirrors `abmio::Status` (include/abmio/status.h); the values must match.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Status {
    Ok = 0,
    Truncated = 1,
    BadHeader = 2,
    BadIndex = 3,
    SizeMismatch = 4,
    Corrupt = 5,
    BadSchema = 6,
}

const MAGIC: &[u8; 4] = b"ABDZ";
const HEADER: usize = 16;
const FLAG_SHUFFLE: u8 = 1;
const FLAG_XOR: u8 = 2;
/// LZ4's maximum input size, as in `src/codec.cc`.
const LZ4_MAX_INPUT_SIZE: u64 = 0x7E00_0000;

/// What a frame's header says, once it has been checked.
struct Header {
    flags: u8,
    decoded_size: usize,
}

fn parse_header(frame: &[u8]) -> Result<Header, Status> {
    if frame.len() < HEADER {
        return Err(Status::Truncated);
    }
    if &frame[0..4] != MAGIC {
        return Err(Status::BadHeader);
    }
    let flags = frame[4];
    if flags & !(FLAG_SHUFFLE | FLAG_XOR) != 0 || frame[5] != 0 || frame[6] != 0 || frame[7] != 0 {
        return Err(Status::BadHeader);
    }
    let mut size = [0u8; 8];
    size.copy_from_slice(&frame[8..16]);
    let decoded_size = u64::from_le_bytes(size);
    let payload = (frame.len() - HEADER) as u64;
    if decoded_size > LZ4_MAX_INPUT_SIZE || payload > i32::MAX as u64 {
        return Err(Status::BadHeader);
    }
    Ok(Header {
        flags,
        decoded_size: decoded_size as usize,
    })
}

/// Reverses the 8-byte shuffle of `src/codec.cc` in place over a copy.
fn unshuffle(input: &[u8], out: &mut [u8]) {
    let n = out.len();
    let groups = n / 8;
    for i in 0..groups {
        for b in 0..8 {
            out[i * 8 + b] = input[b * groups + i];
        }
    }
    out[8 * groups..n].copy_from_slice(&input[8 * groups..n]);
}

/// Decodes one frame, as `abmio::DecodeDelta` does.
///
/// `previous` holds the message this frame was encoded against, if any. On
/// success `out` holds exactly the decoded message; on failure it is empty.
pub fn decode_delta(frame: &[u8], previous: Option<&[u8]>, out: &mut Vec<u8>) -> Status {
    out.clear();
    let header = match parse_header(frame) {
        Ok(h) => h,
        Err(status) => return status,
    };
    let n = header.decoded_size;
    let payload = &frame[HEADER..];

    let xored = header.flags & FLAG_XOR != 0;
    if xored && previous.map_or(true, |p| p.len() != n) {
        return Status::SizeMismatch;
    }
    if n == 0 {
        return if payload.is_empty() {
            Status::Ok
        } else {
            Status::Corrupt
        };
    }
    // Beyond LZ4's maximum ratio: a forged size, as the C++ decoder puts it.
    if n as u64 > payload.len() as u64 * 256 + 64 {
        return Status::Corrupt;
    }

    let mut decoded = vec![0u8; n];
    if !lz4::decompress_exact(payload, &mut decoded) {
        return Status::Corrupt;
    }
    if header.flags & FLAG_SHUFFLE != 0 {
        let shuffled = decoded;
        decoded = vec![0u8; n];
        unshuffle(&shuffled, &mut decoded);
    }
    if xored {
        let previous = previous.unwrap_or(&[]);
        for (byte, p) in decoded.iter_mut().zip(previous) {
            *byte ^= p;
        }
    }
    *out = decoded;
    Status::Ok
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A frame holding `data` as plain literals, with the given flags.
    fn frame(flags: u8, data: &[u8], payload: &[u8]) -> Vec<u8> {
        let mut f = Vec::new();
        f.extend_from_slice(MAGIC);
        f.push(flags);
        f.extend_from_slice(&[0, 0, 0]);
        f.extend_from_slice(&(data.len() as u64).to_le_bytes());
        f.extend_from_slice(payload);
        f
    }

    /// An LZ4 block of literals only (what the encoder produces for
    /// incompressible data).
    fn literals(data: &[u8]) -> Vec<u8> {
        let mut block = Vec::new();
        if data.len() < 15 {
            block.push((data.len() as u8) << 4);
        } else {
            block.push(0xf0);
            let mut rest = data.len() - 15;
            while rest >= 255 {
                block.push(255);
                rest -= 255;
            }
            block.push(rest as u8);
        }
        block.extend_from_slice(data);
        block
    }

    #[test]
    fn a_plain_frame() {
        let data = b"the quick brown fox jumps over the lazy dog";
        let mut out = Vec::new();
        assert_eq!(
            decode_delta(&frame(0, data, &literals(data)), None, &mut out),
            Status::Ok
        );
        assert_eq!(out, data);
    }

    #[test]
    fn an_empty_message() {
        let mut out = Vec::new();
        assert_eq!(
            decode_delta(&frame(0, b"", &[]), None, &mut out),
            Status::Ok
        );
        assert!(out.is_empty());
        // An empty message with a payload is inconsistent.
        assert_eq!(
            decode_delta(&frame(0, b"", &[1]), None, &mut out),
            Status::Corrupt
        );
    }

    #[test]
    fn a_frame_encoded_against_a_previous_message() {
        let previous = vec![0x5au8; 32];
        let current: Vec<u8> = (0..32u8).collect();
        let difference: Vec<u8> = current.iter().zip(&previous).map(|(c, p)| c ^ p).collect();
        let f = frame(FLAG_XOR, &difference, &literals(&difference));
        let mut out = Vec::new();
        assert_eq!(decode_delta(&f, Some(&previous), &mut out), Status::Ok);
        assert_eq!(out, current);
        // Without the previous message, or with one of the wrong size.
        assert_eq!(decode_delta(&f, None, &mut out), Status::SizeMismatch);
        assert_eq!(
            decode_delta(&f, Some(&previous[..31]), &mut out),
            Status::SizeMismatch
        );
    }

    #[test]
    fn malformed_frames_are_rejected() {
        let mut out = Vec::new();
        let data = b"0123456789abcdef";
        let good = frame(0, data, &literals(data));
        assert_eq!(decode_delta(&good[..15], None, &mut out), Status::Truncated);
        let mut wrong_magic = good.clone();
        wrong_magic[0] = b'X';
        assert_eq!(
            decode_delta(&wrong_magic, None, &mut out),
            Status::BadHeader
        );
        let mut unknown_flag = good.clone();
        unknown_flag[4] = 4;
        assert_eq!(
            decode_delta(&unknown_flag, None, &mut out),
            Status::BadHeader
        );
        let mut dirty_padding = good.clone();
        dirty_padding[6] = 1;
        assert_eq!(
            decode_delta(&dirty_padding, None, &mut out),
            Status::BadHeader
        );
        let mut huge = good.clone();
        huge[8..16].copy_from_slice(&u64::MAX.to_le_bytes());
        assert_eq!(decode_delta(&huge, None, &mut out), Status::BadHeader);
        // A size the payload could not possibly produce.
        let mut forged = good.clone();
        forged[8..16].copy_from_slice(&(1u64 << 30).to_le_bytes());
        assert_eq!(decode_delta(&forged, None, &mut out), Status::Corrupt);
        // A plausible size with a block whose literals run past the payload.
        let mut broken = frame(0, data, &literals(data));
        broken.pop();
        assert_eq!(decode_delta(&broken, None, &mut out), Status::Corrupt);
        assert!(out.is_empty(), "a rejected frame leaves no output");
    }

    #[test]
    fn the_shuffle_is_reversed() {
        // Shuffle as the encoder does, then check the decoder undoes it.
        let data: Vec<u8> = (0..37u8).collect();
        let n = data.len();
        let groups = n / 8;
        let mut shuffled = vec![0u8; n];
        for i in 0..groups {
            for b in 0..8 {
                shuffled[b * groups + i] = data[i * 8 + b];
            }
        }
        shuffled[8 * groups..].copy_from_slice(&data[8 * groups..]);
        let f = frame(FLAG_SHUFFLE, &data, &literals(&shuffled));
        let mut out = Vec::new();
        assert_eq!(decode_delta(&f, None, &mut out), Status::Ok);
        assert_eq!(out, data);
    }
}
