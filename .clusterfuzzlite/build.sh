#!/bin/bash -eu
# Builds the fuzz targets inside the ClusterFuzzLite / OSS-Fuzz image, which provides
# $CXX, $CXXFLAGS, $LIB_FUZZING_ENGINE, $SRC and $OUT. LZ4 is linked statically
# because the image that runs the fuzzers does not ship it.
cd "$SRC/abm-io"
lz4_static="${LZ4_STATIC:-/usr/lib/x86_64-linux-gnu/liblz4.a}"
for f in fuzz_unpack fuzz_delta; do
  # shellcheck disable=SC2086  # CXXFLAGS and LIB_FUZZING_ENGINE are intentionally word-split
  $CXX $CXXFLAGS -std=c++17 -Iinclude "fuzz/$f.cc" src/*.cc $LIB_FUZZING_ENGINE "$lz4_static" -o "$OUT/$f"
  (cd "fuzz/corpus/$f" && zip -q -r "$OUT/${f}_seed_corpus.zip" .)
done
