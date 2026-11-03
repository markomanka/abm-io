# The Rust decoder experiment

A bounded experiment (milestone R of [docs/ROADMAP.md](../docs/ROADMAP.md)): abm-io's delta decoder written
again in safe Rust, behind a C interface, so that its memory safety and its round trip can be proved with
Kani, then compared with the C++ decoder by differential fuzzing and benchmarks. It ends with a decision,
recorded in `docs/INTEGRATION.md`: the codec core moves to Rust, or stays in C++ with CBMC and fuzzing.

While the experiment runs, **the C++ decoder is the one that ships**. This crate is built only with
`-DABMIO_RUST_CODEC=ON`, which is off by default, so neither abm-io's users nor BioDynaMo need a Rust
toolchain.

## What it contains

- `src/lz4.rs`: the LZ4 block format, decompression only, in safe Rust. abm-io's frames carry an LZ4 block,
  and decoding one means reading lengths and offsets out of untrusted bytes and copying by them, which is
  where a decoder goes wrong. Writing it here, rather than calling the C library, is what makes the proofs
  worth having. It follows the reference decoder's end-of-block rules, so that it accepts and rejects exactly
  what the C++ decoder does.
- `src/lib.rs`: the frame (`"ABDZ"`, flags, decoded size, LZ4 block), the 8-byte unshuffle, and the XOR
  against the previous message.
- `src/ffi.rs`: the C interface, declared in `include/abmio/rust_codec.h`. The caller owns the output buffer;
  nothing is allocated across the boundary, and the crate is built with `panic = "abort"`, so no panic can
  cross it.

The crate has **no third-party dependencies**: `Cargo.lock` holds the crate itself and nothing else.

- `src/properties.rs`: the properties the decoder must have, written as plain functions that panic if they do
  not hold: decoding never panics whatever the bytes, a rejected frame leaves no output, unshuffling a
  shuffled message restores it, and a frame built from a message decodes back to exactly that message. Kani
  proves them for every input within the bounds the harnesses declare; the same functions also run over
  enumerated inputs in the crate's tests, so they are exercised wherever Kani is not installed.

## Building and testing it

```bash
cargo test                      # the crate's own tests
cargo fmt --check && cargo clippy --all-targets -- -D warnings
cmake -S . -B build -DABMIO_RUST_CODEC=ON && cmake --build build
ctest --test-dir build -L rust  # the two decoders compared on the same inputs
```

The comparison feeds both decoders frames from the encoder, over several shapes of data, and random and
corrupted bytes, and requires the same status and the same output from each.

The proofs need Kani, which brings its own model checker:

```bash
cargo install --locked kani-verifier && cargo kani setup
cargo kani --manifest-path rust/Cargo.toml
```

They run nightly and on pull requests that touch this crate (`.github/workflows/kani.yml`), not on every
change, because they are slow. The bounds are small on purpose: symbolic execution costs grow quickly with
the size of the input, and the decoder's mistakes, if any, live in its first few bytes.
