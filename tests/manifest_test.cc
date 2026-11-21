// SPDX-License-Identifier: Apache-2.0
// The manifest and the pointer: what is written reads back, and the parsers
// accept their fixed shape and nothing else.
#include <gtest/gtest.h>

#include <string>

#include "abmio/manifest.h"

namespace abmio {
namespace {

Status Parse(const std::string& text, Manifest* m, std::string* why) {
  return ParseManifest(text.data(), text.size(), m, why);
}

TEST(Manifest, WhatIsWrittenReadsBack) {
  Manifest m;
  m.checkpoint = "step-000123";
  m.parts = {{"part-00000.h5", 4096},
             {"part-00001.h5", 0},
             {"part-00002.h5", UINT64_MAX}};
  const std::string text = ToJson(m);
  Manifest back;
  std::string why;
  ASSERT_EQ(Parse(text, &back, &why), Status::kOk) << why << "\n" << text;
  EXPECT_TRUE(back == m);
  EXPECT_EQ(ToJson(back), text) << "the same manifest gives the same text";

  Manifest empty;
  empty.checkpoint = "first";
  ASSERT_EQ(Parse(ToJson(empty), &back, &why), Status::kOk) << why;
  EXPECT_TRUE(back == empty);
}

TEST(Manifest, KeysMayComeInAnyOrderWithAnySpacing) {
  Manifest m;
  std::string why;
  ASSERT_EQ(Parse("\n{ \"parts\" : [ { \"bytes\":7 ,\"name\":\"p.h5\" } ] ,"
                  "\t\"checkpoint\":\"c\",\"abmio_manifest\":1 }\n",
                  &m, &why),
            Status::kOk)
      << why;
  EXPECT_EQ(m.checkpoint, "c");
  ASSERT_EQ(m.parts.size(), 1u);
  EXPECT_EQ(m.parts[0].bytes, 7u);
}

TEST(Manifest, AnythingElseIsRefused) {
  const char* const bad[] = {
      "",
      "{}",
      "[]",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\"}",  // no parts
      "{\"abmio_manifest\": 1, \"parts\": []}",          // no checkpoint
      "{\"checkpoint\": \"c\", \"parts\": []}",          // no version
      "{\"abmio_manifest\": 0, \"checkpoint\": \"c\", \"parts\": []}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [], "
      "\"extra\": 1}",
      "{\"abmio_manifest\": 1, \"abmio_manifest\": 1, \"checkpoint\": \"c\", "
      "\"parts\": []}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"../etc\", \"parts\": []}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"latest\", \"parts\": []}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"/abs.h5\", \"bytes\": 1}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"a\\\"b\", \"bytes\": 1}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\", \"bytes\": -1}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\", \"bytes\": 1.5}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\", \"bytes\": 01}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\", \"bytes\": 18446744073709551616}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\", \"bytes\": 1}, {\"name\": \"p\", \"bytes\": 2}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [{\"name\": "
      "\"p\"}]}",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": []} trailing",
      "{\"abmio_manifest\": 1, \"checkpoint\": \"c\", \"parts\": [,]}",
  };
  for (const char* text : bad) {
    Manifest m;
    std::string why;
    EXPECT_EQ(Parse(text, &m, &why), Status::kCorrupt) << text;
    EXPECT_FALSE(why.empty()) << text;
  }
}

TEST(Manifest, ANewerFormatIsRefusedWithItsVersion) {
  Manifest m;
  std::string why;
  EXPECT_EQ(
      Parse("{\"abmio_manifest\": 2, \"checkpoint\": \"c\", \"parts\": []}", &m,
            &why),
      Status::kBadHeader);
  EXPECT_NE(why.find("format 2"), std::string::npos) << why;
}

TEST(Manifest, NamesCannotLeaveTheirDirectory) {
  for (const char* good : {"step-1", "part-00000.h5", "a", "A_b.c-9"}) {
    EXPECT_TRUE(IsCheckpointName(good)) << good;
  }
  for (const char* bad : {"", ".", "..", ".hidden", "a/b", "a\\b", "latest",
                          "x.tmp", "spa ce", "\xc3\xa9"}) {
    EXPECT_FALSE(IsCheckpointName(bad)) << bad;
  }
  EXPECT_FALSE(IsCheckpointName(std::string(129, 'a')));
  EXPECT_TRUE(IsCheckpointName(std::string(128, 'a')));
}

TEST(Pointer, WhatIsWrittenReadsBackAndNothingElse) {
  const std::string text = PointerJson("step-7");
  std::string name, why;
  ASSERT_EQ(ParsePointer(text.data(), text.size(), &name, &why), Status::kOk)
      << why;
  EXPECT_EQ(name, "step-7");
  for (const std::string& bad :
       {std::string("{\"abmio_pointer\": 1}"),
        std::string("{\"abmio_pointer\": 1, \"checkpoint\": \"..\"}"),
        std::string("{\"checkpoint\": \"a\"}"),
        std::string("{\"abmio_pointer\": 1, \"checkpoint\": \"a\", \"b\": 1}"),
        std::string("{\"abmio_pointer\": 1, \"checkpoint\": \"a\"")}) {
    EXPECT_EQ(ParsePointer(bad.data(), bad.size(), &name, &why),
              Status::kCorrupt)
        << bad;
  }
  const std::string newer = "{\"abmio_pointer\": 9, \"checkpoint\": \"a\"}";
  EXPECT_EQ(ParsePointer(newer.data(), newer.size(), &name, &why),
            Status::kBadHeader);
}

}  // namespace
}  // namespace abmio
