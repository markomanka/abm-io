#!/bin/bash -eu
# Builds the fuzz targets inside the ClusterFuzzLite / OSS-Fuzz image, which provides
# $CXX, $CXXFLAGS, $LIB_FUZZING_ENGINE, $SRC and $OUT. LZ4 is linked statically
# because the image that runs the fuzzers does not ship it.
cd "$SRC/abm-io"
lz4_static="${LZ4_STATIC:-/usr/lib/x86_64-linux-gnu/liblz4.a}"
# src/h5lz4.cc registers the filter with HDF5, which the image does not have; the
# fuzzers need only the filter's codec, src/h5lz4_codec.cc.
# Only src/*.cc: the checkpoint container, in src/container, needs HDF5, which
# this image does not have, and h5lz4.cc is the filter's registration with it.
sources=$(ls src/*.cc | grep -v '^src/h5lz4\.cc$')
for f in fuzz_unpack fuzz_delta fuzz_h5lz4; do
  # shellcheck disable=SC2086  # CXXFLAGS, LIB_FUZZING_ENGINE and sources are intentionally word-split
  $CXX $CXXFLAGS -std=c++17 -Iinclude "fuzz/$f.cc" $sources $LIB_FUZZING_ENGINE "$lz4_static" -o "$OUT/$f"
  (cd "fuzz/corpus/$f" && zip -q -r "$OUT/${f}_seed_corpus.zip" .)
done
