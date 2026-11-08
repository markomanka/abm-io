// SPDX-License-Identifier: Apache-2.0
//! Whether to compress, decided from declared inputs.
//!
//! Compressing is not free: on storage faster than the compressor it makes
//! writing slower (`docs/INTEGRATION.md`, "Checkpoint files"). The decision is
//! therefore computed from inputs a run can state and record, never from live
//! timings, so that two runs of the same simulation on the same machine write
//! the same bytes.
//!
//! The properties this must have are in `properties.rs` and proved by Kani:
//! the function is total, it never panics, it is monotone in bandwidth and in
//! the ratio, and the two settings it can return leave the restored data
//! identical, which is what makes the policy safe to retune later.

/// What the writer was asked to do.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Request {
    /// Never compress.
    Off,
    /// Always compress.
    On,
    /// Decide from the inputs below.
    Auto,
}

/// What the writer will do, and what it recorded in the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    Uncompressed,
    Lz4,
}

/// The declared inputs of an `Auto` decision, all measured or configured
/// before writing and recorded with the result.
#[derive(Clone, Copy, Debug)]
pub struct Inputs {
    /// Storage bandwidth available to this writer, in bytes per second, as
    /// configured by the site or measured once at start-up. Zero means
    /// unknown, which is treated as slow storage.
    pub storage_bytes_per_second: u64,
    /// The compressor's throughput on this machine, in bytes per second,
    /// measured on the first chunks. Zero means unknown.
    pub compress_bytes_per_second: u64,
    /// Compressed size per 256 bytes of input, observed on the first chunks:
    /// 256 means no compression at all, 32 means eight to one. Zero means
    /// not yet observed, and is read like the other unknowns below.
    pub compressed_per_256: u16,
}

impl Inputs {
    /// Compressing wins when the time to compress and then write the smaller
    /// payload is less than the time to write the whole of it:
    ///
    /// ```text
    ///     n / compress + n * ratio / storage  <  n / storage
    /// ```
    ///
    /// which, with `ratio = compressed_per_256 / 256`, rearranges to a
    /// comparison of two products and needs no division:
    ///
    /// ```text
    ///     storage * 256  <  compress * (256 - compressed_per_256)
    /// ```
    ///
    /// Every unknown input leans the same way, towards compressing: the cost
    /// of compressing storage that turns out to be fast is some wasted time,
    /// while the cost of not compressing storage that is slow is a simulation
    /// waiting on its checkpoint. Leaning one way for one unknown and the other
    /// way for another also breaks monotonicity, which the proofs reject.
    ///
    /// The rule is written in two steps on purpose. The right-hand side
    /// depends only on the compressor and the ratio, and is computed once as
    /// a [`Threshold`]; the storage bandwidth is then compared against it by
    /// [`pays_against`]. The split lets the monotonicity proof take the
    /// threshold as a symbolic number instead of making the solver build the
    /// multiplier that produces it, which is what put the proof past its time
    /// limit (`docs/RATIONALE.md`, entry 9). This is the first step, the
    /// part of the rule that does not depend on the storage bandwidth.
    pub fn threshold(&self) -> Threshold {
        if self.compress_bytes_per_second == 0 || self.compressed_per_256 == 0 {
            return Threshold::Compress; // unknown: lean towards compressing
        }
        let ratio = self.compressed_per_256;
        if ratio >= 256 {
            return Threshold::Never; // the data does not compress: never pay for it
        }
        Threshold::Compare(u128::from(self.compress_bytes_per_second) * u128::from(256 - ratio))
    }
}

/// The right-hand side of the comparison, from the compressor and the ratio
/// alone: the two cases decided without arithmetic, and the product
/// `compress * (256 - ratio)` otherwise.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Threshold {
    /// An unknown input: compress.
    Compress,
    /// Data that does not compress: never.
    Never,
    /// Compare `storage * 256` against this.
    Compare(u128),
}

/// Whether compressing pays at `storage` bytes per second against a
/// threshold. Unknown storage (zero) is treated as slow storage.
pub fn pays_against(storage: u64, threshold: Threshold) -> bool {
    if storage == 0 {
        return true;
    }
    match threshold {
        Threshold::Compress => true,
        Threshold::Never => false,
        Threshold::Compare(compressing_then_writing) => {
            u128::from(storage) * 256 < compressing_then_writing
        }
    }
}

/// The decision, and the only place it is made.
pub fn decide(request: Request, inputs: Inputs) -> Decision {
    decide_against(request, inputs.storage_bytes_per_second, inputs.threshold())
}

/// The decision with the threshold already computed. `decide` is this
/// function applied to `inputs.threshold()`, so a property proved of it for
/// every threshold holds of `decide` for every input, by substitution and
/// without a solver.
pub fn decide_against(request: Request, storage: u64, threshold: Threshold) -> Decision {
    match request {
        Request::Off => Decision::Uncompressed,
        Request::On => Decision::Lz4,
        Request::Auto => {
            if pays_against(storage, threshold) {
                Decision::Lz4
            } else {
                Decision::Uncompressed
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs(storage: u64, compress: u64, ratio: u16) -> Inputs {
        Inputs {
            storage_bytes_per_second: storage,
            compress_bytes_per_second: compress,
            compressed_per_256: ratio,
        }
    }

    #[test]
    fn the_request_is_obeyed_when_it_is_explicit() {
        for i in [
            inputs(0, 0, 0),
            inputs(10_000_000_000, 100, 255),
            inputs(1, 1 << 40, 1),
        ] {
            assert_eq!(decide(Request::Off, i), Decision::Uncompressed);
            assert_eq!(decide(Request::On, i), Decision::Lz4);
        }
    }

    #[test]
    fn slow_storage_compresses_and_fast_storage_does_not() {
        // A shared file system at 300 MB/s, LZ4 at 500 MB/s, eight to one.
        assert_eq!(
            decide(Request::Auto, inputs(300 << 20, 500 << 20, 32)),
            Decision::Lz4
        );
        // Local NVMe at 5 GB/s: writing the whole of it is faster.
        assert_eq!(
            decide(Request::Auto, inputs(5000 << 20, 500 << 20, 32)),
            Decision::Uncompressed
        );
    }

    #[test]
    fn unknown_inputs_compress() {
        assert_eq!(
            decide(Request::Auto, inputs(0, 500 << 20, 32)),
            Decision::Lz4
        );
        assert_eq!(
            decide(Request::Auto, inputs(300 << 20, 0, 32)),
            Decision::Lz4
        );
        // An unknown ratio leans the same way as the other unknowns, which is
        // also what keeps the decision monotone in the ratio.
        assert_eq!(
            decide(Request::Auto, inputs(5000 << 20, 500 << 20, 0)),
            Decision::Lz4
        );
    }

    #[test]
    fn incompressible_data_is_never_compressed() {
        for storage in [1u64, 1 << 20, 1 << 40] {
            assert_eq!(
                decide(Request::Auto, inputs(storage, 1 << 20, 256)),
                Decision::Uncompressed
            );
        }
    }
}
