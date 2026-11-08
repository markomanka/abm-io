// SPDX-License-Identifier: Apache-2.0
// The LZ4 filter registered with HDF5: a dataset written and read through
// HDF5 with the filter declared, which is what docs/FINDINGS.md measured
// before the container existed. The codec on its own is
// tests/h5lz4_codec_test.cc.
#include <gtest/gtest.h>
#include <hdf5.h>

#include <cstdio>
#include <string>
#include <vector>

#include "abmio/h5lz4.h"

namespace abmio {
namespace {

TEST(H5Lz4, RegistersAndRoundTripsThroughHdf5) {
  ASSERT_TRUE(RegisterLz4Filter());
  EXPECT_TRUE(Lz4FilterAvailable());
  EXPECT_TRUE(RegisterLz4Filter());  // idempotent

  const std::string path = "abmio_h5lz4_test.h5";
  std::vector<double> in(8192);
  for (std::size_t i = 0; i < in.size(); ++i) {
    in[i] = static_cast<double>(i % 100) * 0.5;
  }
  {
    hid_t file =
        H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    ASSERT_GE(file, 0);
    hsize_t dims = in.size();
    hsize_t chunk = 1024;
    hid_t space = H5Screate_simple(1, &dims, nullptr);
    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(dcpl, 1, &chunk);
    ASSERT_GE(H5Pset_filter(dcpl, kLz4FilterId, H5Z_FLAG_MANDATORY, 0, nullptr),
              0);
    hid_t set = H5Dcreate2(file, "x", H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                           dcpl, H5P_DEFAULT);
    ASSERT_GE(set, 0);
    ASSERT_GE(H5Dwrite(set, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       in.data()),
              0);
    H5Dclose(set);
    H5Pclose(dcpl);
    H5Sclose(space);
    H5Fclose(file);
  }
  {
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(file, 0);
    hid_t set = H5Dopen2(file, "x", H5P_DEFAULT);
    ASSERT_GE(set, 0);
    std::vector<double> out(in.size());
    ASSERT_GE(H5Dread(set, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      out.data()),
              0);
    EXPECT_EQ(out, in);
    hid_t dcpl = H5Dget_create_plist(set);
    unsigned flags = 0;
    size_t nelmts = 0;
    EXPECT_EQ(
        H5Pget_filter2(dcpl, 0, &flags, &nelmts, nullptr, 0, nullptr, nullptr),
        static_cast<H5Z_filter_t>(kLz4FilterId));
    hsize_t stored = H5Dget_storage_size(set);
    EXPECT_LT(stored, in.size() * sizeof(double) / 4);
    H5Pclose(dcpl);
    H5Dclose(set);
    H5Fclose(file);
  }
  std::remove(path.c_str());
}

}  // namespace
}  // namespace abmio
