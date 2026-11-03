// SPDX-License-Identifier: Apache-2.0
//! The LZ4 block format, decompression only, in safe Rust.
//!
//! abm-io's frames carry an LZ4 block (`src/codec.cc`). Decoding one means
//! reading lengths and offsets out of untrusted bytes and copying by them,
//! which is where a decoder goes wrong; here every access is bounds-checked
//! by the language, and the whole function is proved panic-free and
//! bounds-safe (milestone R).
//!
//! Like `LZ4_decompress_safe`, this reads exactly `src` and writes exactly
//! `dst`: it fails unless the block ends precisely at the end of the input
//! and fills the output exactly.

/// Decompresses `src` into `dst`, which must be exactly the decoded size.
/// Returns false, leaving `dst` partly written, if the block is malformed.
///
/// The end-of-block rules are those the reference decoder enforces: the block
/// ends with a sequence of literals alone, which must finish exactly at the
/// end of the input, and a sequence that carries a match must leave at least
/// `MF_LIMIT` bytes of output and `8` bytes of input after its literals.
pub fn decompress_exact(src: &[u8], dst: &mut [u8]) -> bool {
    const MF_LIMIT: usize = 12;

    let mut i: usize = 0; // read position in src
    let mut o: usize = 0; // write position in dst

    loop {
        // Token: high four bits the literal length, low four the match length.
        let Some(&token) = src.get(i) else {
            return false;
        };
        i += 1;

        let mut literals = (token >> 4) as usize;
        if literals == 15 {
            loop {
                let Some(&more) = src.get(i) else {
                    return false;
                };
                i += 1;
                let Some(sum) = literals.checked_add(more as usize) else {
                    return false;
                };
                literals = sum;
                if more != 255 {
                    break;
                }
            }
        }

        // Is this the last sequence? The reference decoder decides by the room
        // left in the output and the input, before copying anything.
        let Some(output_end) = o.checked_add(literals) else {
            return false;
        };
        let Some(input_end) = i.checked_add(literals) else {
            return false;
        };
        let last = output_end + MF_LIMIT > dst.len() || input_end + 8 > src.len();

        if last {
            // It must consume the input exactly and fill the output exactly.
            if input_end != src.len() || output_end != dst.len() {
                return false;
            }
            dst[o..output_end].copy_from_slice(&src[i..input_end]);
            return true;
        }

        dst[o..output_end].copy_from_slice(&src[i..input_end]);
        i = input_end;
        o = output_end;

        // Match: a two-byte little-endian offset back into the output.
        let (Some(&low), Some(&high)) = (src.get(i), src.get(i + 1)) else {
            return false;
        };
        i += 2;
        let offset = u16::from_le_bytes([low, high]) as usize;
        if offset == 0 || offset > o {
            return false;
        }

        let mut length = (token & 0x0f) as usize;
        if length == 15 {
            loop {
                let Some(&more) = src.get(i) else {
                    return false;
                };
                i += 1;
                let Some(sum) = length.checked_add(more as usize) else {
                    return false;
                };
                length = sum;
                if more != 255 {
                    break;
                }
            }
        }
        let Some(length) = length.checked_add(4) else {
            return false;
        }; // the minimum match
        if length > dst.len() - o {
            return false;
        }

        // Copy from earlier output, one byte at a time: the ranges may overlap,
        // which is how LZ4 encodes runs.
        for k in 0..length {
            dst[o + k] = dst[o + k - offset];
        }
        o += length;
    }
}

#[cfg(test)]
mod tests {
    use super::decompress_exact;

    /// An LZ4 block holding `literals` and nothing else.
    fn only_literals(literals: &[u8]) -> Vec<u8> {
        let mut block = Vec::new();
        if literals.len() < 15 {
            block.push((literals.len() as u8) << 4);
        } else {
            block.push(0xf0);
            let mut rest = literals.len() - 15;
            while rest >= 255 {
                block.push(255);
                rest -= 255;
            }
            block.push(rest as u8);
        }
        block.extend_from_slice(literals);
        block
    }

    #[test]
    fn literals_of_every_length() {
        for n in [0usize, 1, 14, 15, 16, 269, 270, 600] {
            let data: Vec<u8> = (0..n).map(|i| (i % 251) as u8).collect();
            let mut out = vec![0u8; n];
            assert!(
                decompress_exact(&only_literals(&data), &mut out),
                "length {n}"
            );
            assert_eq!(out, data, "length {n}");
        }
    }

    #[test]
    fn a_match_repeats_earlier_output() {
        // "ab", a match of six bytes at offset two, then twelve final literals:
        // the end-of-block rules leave no match near the end.
        let mut block = vec![0x22, b'a', b'b', 2, 0, 0xc0];
        block.extend_from_slice(b"0123456789ab");
        let mut out = vec![0u8; 20];
        assert!(decompress_exact(&block, &mut out));
        assert_eq!(&out, b"abababab0123456789ab");
    }

    #[test]
    fn a_block_ending_with_a_match_is_rejected() {
        // What the reference decoder rejects: no final literals.
        let block = [0x22, b'a', b'b', 2, 0];
        assert!(!decompress_exact(&block, &mut [0u8; 8]));
    }

    #[test]
    fn malformed_blocks_are_rejected() {
        let mut out = [0u8; 8];
        let cases: &[(&str, &[u8])] = &[
            ("empty input", &[]),
            ("literal length runs past the input", &[0x80, b'a']),
            ("truncated length extension", &[0xf0]),
            ("offset of zero", &[0x24, b'a', b'b', 0, 0]),
            (
                "offset before the start of the output",
                &[0x24, b'a', b'b', 9, 0],
            ),
            ("truncated offset", &[0x24, b'a', b'b', 2]),
            (
                "match longer than the output",
                &[0x2f, b'a', b'b', 2, 0, 255, 255],
            ),
        ];
        for (name, block) in cases {
            assert!(
                !decompress_exact(block, &mut out),
                "{name} should be rejected"
            );
        }
        // Exactly the right length is required, no more and no less.
        let block = only_literals(b"abcdefgh");
        assert!(!decompress_exact(&block, &mut [0u8; 7]));
        assert!(!decompress_exact(&block, &mut [0u8; 9]));
        assert!(decompress_exact(&block, &mut [0u8; 8]));
    }
}
