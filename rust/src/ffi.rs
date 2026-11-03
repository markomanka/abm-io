// SPDX-License-Identifier: Apache-2.0
//! The C interface, through which C++ calls the decoder.
//!
//! The caller owns the output buffer: `abmio_rust_decode_delta` writes at most
//! `out_capacity` bytes and reports the size it needed, so a caller that
//! guessed too small can retry. Nothing is allocated across the boundary, and
//! no panic can cross it: the crate is built with `panic = "abort"`.

use crate::{decode_delta, Status};

/// Decodes one frame into a caller-owned buffer.
///
/// Returns a value of `abmio::Status`. On success `*out_size` is the decoded
/// size, and that many bytes have been written to `out`, unless the buffer was
/// too small, in which case the status is `kSizeMismatch` and `*out_size` is
/// the size the caller must provide.
///
/// # Safety
///
/// `frame` must point to `frame_size` readable bytes, `previous` to
/// `previous_size` readable bytes or be null, `out` to `out_capacity` writable
/// bytes or be null when `out_capacity` is zero, and `out_size` must be a
/// valid pointer.
#[no_mangle]
pub unsafe extern "C" fn abmio_rust_decode_delta(
    frame: *const u8,
    frame_size: usize,
    previous: *const u8,
    previous_size: usize,
    out: *mut u8,
    out_capacity: usize,
    out_size: *mut usize,
) -> u8 {
    if frame.is_null() || out_size.is_null() || (out.is_null() && out_capacity != 0) {
        return Status::BadHeader as u8;
    }
    // SAFETY: the caller guarantees the sizes of the buffers above.
    let frame = unsafe { std::slice::from_raw_parts(frame, frame_size) };
    let previous = if previous.is_null() {
        None
    } else {
        Some(unsafe { std::slice::from_raw_parts(previous, previous_size) })
    };

    let mut decoded = Vec::new();
    let status = decode_delta(frame, previous, &mut decoded);
    // SAFETY: out_size is a valid pointer, checked above.
    unsafe { *out_size = decoded.len() };
    if status != Status::Ok {
        return status as u8;
    }
    if decoded.len() > out_capacity {
        return Status::SizeMismatch as u8;
    }
    if !decoded.is_empty() {
        // SAFETY: out holds out_capacity writable bytes, and decoded fits.
        unsafe { std::ptr::copy_nonoverlapping(decoded.as_ptr(), out, decoded.len()) };
    }
    Status::Ok as u8
}
