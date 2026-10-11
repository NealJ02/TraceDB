#include "util/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace tracedb {
namespace {

constexpr uint32_t kMax32 = std::numeric_limits<uint32_t>::max();
constexpr uint64_t kMax64 = std::numeric_limits<uint64_t>::max();

// Builds a byte string from a list of byte values, e.g. Bytes({0x2c, 0x01}).
std::string Bytes(std::initializer_list<uint8_t> bytes) {
  std::string s;
  for (uint8_t b : bytes) s.push_back(static_cast<char>(b));
  return s;
}

// ---------------------------------------------------------------------------
// Fixed32 / Fixed64 (little-endian)
// ---------------------------------------------------------------------------

TEST(Fixed32Test, KnownBytes) {
  std::string s;
  PutFixed32(&s, 300);
  EXPECT_EQ(s, Bytes({0x2c, 0x01, 0x00, 0x00}));
}

TEST(Fixed32Test, RoundTrip) {
  // 0x80000000 has its top byte = 0x80, which catches shifting a promoted
  // signed int (undefined behavior) in the decoder.
  for (uint32_t v : {0u, 1u, 0xffu, 0x100u, 300u, 0x80000000u, kMax32}) {
    std::string s;
    PutFixed32(&s, v);
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(DecodeFixed32(s.data()), v) << "value=" << v;
  }
}

TEST(Fixed32Test, GetAdvancesInput) {
  std::string s = Bytes({0x2c, 0x01, 0x00, 0x00, 0xff});
  std::string_view input = s;
  uint32_t v = 0;
  ASSERT_TRUE(GetFixed32(&input, &v));
  EXPECT_EQ(v, 300u);
  EXPECT_EQ(input, Bytes({0xff}));
}

TEST(Fixed32Test, GetFailsOnShortInputAndLeavesItUnchanged) {
  std::string s = Bytes({0x2c, 0x01, 0x00});
  std::string_view input = s;
  uint32_t v = 0;
  EXPECT_FALSE(GetFixed32(&input, &v));
  EXPECT_EQ(input.size(), 3u);
}

TEST(Fixed64Test, KnownBytes) {
  std::string s;
  PutFixed64(&s, 0x0102030405060708ULL);
  EXPECT_EQ(s, Bytes({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
}

TEST(Fixed64Test, RoundTrip) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{kMax32},
                     uint64_t{1} << 32, uint64_t{1} << 63,
                     uint64_t{0xfedcba9876543210ULL}, kMax64}) {
    std::string s;
    PutFixed64(&s, v);
    ASSERT_EQ(s.size(), 8u);
    std::string_view input = s;
    uint64_t decoded = 0;
    ASSERT_TRUE(GetFixed64(&input, &decoded));
    EXPECT_EQ(decoded, v);
    EXPECT_TRUE(input.empty());
  }
}

TEST(Fixed64Test, GetFailsOnShortInputAndLeavesItUnchanged) {
  std::string s(7, '\0');
  std::string_view input = s;
  uint64_t v = 0;
  EXPECT_FALSE(GetFixed64(&input, &v));
  EXPECT_EQ(input.size(), 7u);
}

// ---------------------------------------------------------------------------
// Big-endian 64
// ---------------------------------------------------------------------------

TEST(BigEndian64Test, KnownBytes) {
  std::string s;
  PutBigEndian64(&s, 0x0102030405060708ULL);
  EXPECT_EQ(s, Bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
}

TEST(BigEndian64Test, RoundTrip) {
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{256},
                     uint64_t{1} << 63, uint64_t{1791244800000000000ULL},
                     kMax64}) {
    char buf[8];
    EncodeBigEndian64(buf, v);
    EXPECT_EQ(DecodeBigEndian64(buf), v);
  }
}

// The reason big-endian exists: comparing the encoded bytes must give the same
// order as comparing the numbers. Little-endian fails this (e.g. 1 vs 256).
TEST(BigEndian64Test, ByteOrderMatchesNumericOrder) {
  auto encode = [](uint64_t v) {
    std::string s;
    PutBigEndian64(&s, v);
    return s;
  };

  // The little-endian counterexample: 256 > 1, but its low byte is smaller.
  EXPECT_LT(encode(1), encode(256));

  std::mt19937_64 rng(/*seed=*/42);
  for (int i = 0; i < 10000; ++i) {
    uint64_t a = rng();
    // Half the time, make b differ from a only in the low byte, so the
    // comparison is decided by the last byte rather than the first.
    uint64_t b = (i % 2 == 0) ? rng() : (a & ~uint64_t{0xff}) | (rng() & 0xff);
    EXPECT_EQ(a < b, encode(a) < encode(b)) << "a=" << a << " b=" << b;
    EXPECT_EQ(a == b, encode(a) == encode(b)) << "a=" << a << " b=" << b;
  }
}

// ---------------------------------------------------------------------------
// Varint64
// ---------------------------------------------------------------------------

TEST(Varint64Test, KnownBytes) {
  struct Case {
    uint64_t value;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {0, Bytes({0x00})},
      {1, Bytes({0x01})},
      {127, Bytes({0x7f})},
      {128, Bytes({0x80, 0x01})},
      {300, Bytes({0xac, 0x02})},
      {1250000000, Bytes({0x80, 0xf9, 0x85, 0xd4, 0x04})},
      {kMax64,
       Bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})},
  };
  for (const Case& c : cases) {
    std::string s;
    PutVarint64(&s, c.value);
    EXPECT_EQ(s, c.bytes) << "value=" << c.value;
  }
}

// Every point where the encoding grows by one byte, plus a value just below.
TEST(Varint64Test, RoundTripAtLengthBoundaries) {
  std::vector<uint64_t> values = {0, kMax64};
  for (int bits = 7; bits < 64; bits += 7) {
    uint64_t boundary = uint64_t{1} << bits;  // 128, 16384, ...
    values.push_back(boundary - 1);           // largest value with N bytes
    values.push_back(boundary);               // smallest value with N+1 bytes
  }
  values.push_back(uint64_t{1} << 63);

  for (uint64_t v : values) {
    std::string s;
    PutVarint64(&s, v);
    EXPECT_EQ(s.size(), VarintLength(v)) << "value=" << v;
    EXPECT_LE(s.size(), kMaxVarint64Length);

    std::string_view input = s;
    uint64_t decoded = 0;
    ASSERT_TRUE(GetVarint64(&input, &decoded)) << "value=" << v;
    EXPECT_EQ(decoded, v);
    EXPECT_TRUE(input.empty());
  }
}

TEST(Varint64Test, VarintLengthValues) {
  EXPECT_EQ(VarintLength(0), 1u);
  EXPECT_EQ(VarintLength(127), 1u);
  EXPECT_EQ(VarintLength(128), 2u);
  EXPECT_EQ(VarintLength(16383), 2u);
  EXPECT_EQ(VarintLength(16384), 3u);
  EXPECT_EQ(VarintLength(kMax64), 10u);
}

TEST(Varint64Test, ReadsSequentialValues) {
  std::string s;
  PutVarint64(&s, 1);
  PutVarint64(&s, 300);
  PutVarint64(&s, kMax64);

  std::string_view input = s;
  uint64_t v = 0;
  ASSERT_TRUE(GetVarint64(&input, &v));
  EXPECT_EQ(v, 1u);
  ASSERT_TRUE(GetVarint64(&input, &v));
  EXPECT_EQ(v, 300u);
  ASSERT_TRUE(GetVarint64(&input, &v));
  EXPECT_EQ(v, kMax64);
  EXPECT_TRUE(input.empty());
}

// Each of these inputs is invalid. Decoding must fail and leave the input
// untouched.
TEST(Varint64Test, RejectsMalformedInput) {
  struct Case {
    const char* why;
    std::string bytes;
  };
  const std::string ten_ff(10, '\xff');
  std::string overflow(9, '\xff');
  overflow.push_back('\x02');  // 10th byte may only be 0 or 1
  std::string tenth_continues(9, '\xff');
  tenth_continues.push_back('\x81');  // continuation bit set on 10th byte

  const std::vector<Case> cases = {
      {"empty", ""},
      {"continuation bit with nothing after", Bytes({0x80})},
      {"truncated 2-byte varint", Bytes({0xff, 0xff})},
      {"10 bytes still continuing", ten_ff},
      {"11 bytes", ten_ff + '\x01'},
      {"10th byte overflows 64 bits", overflow},
      {"10th byte has continuation bit", tenth_continues},
  };
  for (const Case& c : cases) {
    std::string_view input = c.bytes;
    uint64_t v = 12345;
    EXPECT_FALSE(GetVarint64(&input, &v)) << c.why;
    EXPECT_EQ(input.size(), c.bytes.size()) << c.why;
    EXPECT_EQ(v, 12345u) << c.why;  // output not written on failure
  }
}

// Cutting a valid varint short at any point must fail, never over-read.
TEST(Varint64Test, EveryTruncationFails) {
  std::string s;
  PutVarint64(&s, kMax64);  // 10 bytes
  for (size_t len = 0; len < s.size(); ++len) {
    std::string_view input(s.data(), len);
    uint64_t v = 0;
    EXPECT_FALSE(GetVarint64(&input, &v)) << "len=" << len;
  }
}

// ---------------------------------------------------------------------------
// Length-prefixed strings
// ---------------------------------------------------------------------------

TEST(LengthPrefixedTest, KnownBytes) {
  std::string s;
  PutLengthPrefixed(&s, "hi");
  EXPECT_EQ(s, Bytes({0x02, 'h', 'i'}));
}

TEST(LengthPrefixedTest, RoundTrip) {
  const std::vector<std::string> values = {
      "",
      "llm_call",
      std::string("a\0b\xff", 4),  // binary bytes, including 0x00 and 0xff
      std::string(20 * 1024, 'x'),  // 20 KiB payload: 2-byte length prefix
  };
  for (const std::string& value : values) {
    std::string s;
    PutLengthPrefixed(&s, value);
    EXPECT_EQ(s.size(), VarintLength(value.size()) + value.size());

    std::string_view input = s;
    std::string_view result;
    ASSERT_TRUE(GetLengthPrefixed(&input, &result));
    EXPECT_EQ(result, value);
    EXPECT_TRUE(input.empty());
  }
}

TEST(LengthPrefixedTest, ResultIsAViewIntoTheInput) {
  std::string s;
  PutLengthPrefixed(&s, "hello");
  std::string_view input = s;
  std::string_view result;
  ASSERT_TRUE(GetLengthPrefixed(&input, &result));
  // Points into s itself (just past the 1-byte length), not a copy.
  EXPECT_EQ(result.data(), s.data() + 1);
}

TEST(LengthPrefixedTest, ReadsSequentialValues) {
  std::string s;
  PutLengthPrefixed(&s, "model");
  PutLengthPrefixed(&s, "m1");
  s.push_back('\xff');

  std::string_view input = s;
  std::string_view a, b;
  ASSERT_TRUE(GetLengthPrefixed(&input, &a));
  ASSERT_TRUE(GetLengthPrefixed(&input, &b));
  EXPECT_EQ(a, "model");
  EXPECT_EQ(b, "m1");
  EXPECT_EQ(input, Bytes({0xff}));
}

TEST(LengthPrefixedTest, FailsWhenLengthExceedsRemainingBytes) {
  // Length says 5, but only 2 bytes follow. Input must be left unchanged,
  // including the length byte that was successfully read.
  std::string s = Bytes({0x05, 'h', 'i'});
  std::string_view input = s;
  std::string_view result;
  EXPECT_FALSE(GetLengthPrefixed(&input, &result));
  EXPECT_EQ(input.size(), 3u);
}

TEST(LengthPrefixedTest, FailsOnHugeCorruptedLength) {
  // A length near 2^63 must be rejected, not used to build a view.
  std::string s;
  PutVarint64(&s, uint64_t{1} << 62);
  s += "abc";
  std::string_view input = s;
  std::string_view result;
  EXPECT_FALSE(GetLengthPrefixed(&input, &result));
  EXPECT_EQ(input.size(), s.size());
}

TEST(LengthPrefixedTest, FailsOnMalformedLength) {
  std::string s = Bytes({0x80});  // varint that never finishes
  std::string_view input = s;
  std::string_view result;
  EXPECT_FALSE(GetLengthPrefixed(&input, &result));
  EXPECT_EQ(input.size(), 1u);
}

TEST(LengthPrefixedTest, EveryTruncationFails) {
  std::string s;
  PutLengthPrefixed(&s, std::string(200, 'x'));  // 2-byte length + 200 bytes
  for (size_t len = 0; len < s.size(); ++len) {
    std::string_view input(s.data(), len);
    std::string_view result;
    EXPECT_FALSE(GetLengthPrefixed(&input, &result)) << "len=" << len;
    EXPECT_EQ(input.size(), len) << "len=" << len;
  }
}

}  // namespace
}  // namespace tracedb
