#include "span_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <string_view>

#include "util/coding.h"

namespace tracedb {
namespace {

constexpr uint64_t kMax64 = std::numeric_limits<uint64_t>::max();

std::string Bytes(std::initializer_list<uint8_t> bytes) {
  std::string s;
  for (uint8_t b : bytes) s.push_back(static_cast<char>(b));
  return s;
}

// The span used in the design discussion, whose exact bytes were worked out
// by hand.
Span ExampleSpan() {
  Span span;
  span.key.timestamp_ns = 1791244800000000000ULL;  // 2026-10-06 00:00:00 UTC
  span.key.trace_id = {0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3, 0x4d, 0xa6,
                       0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36};
  span.key.span_id = 0x00f067aa0ba902b7ULL;
  span.parent_id = 0;
  span.name = "llm_call";
  span.duration_ns = 1250000000;  // 1.25 s
  span.tags = {{"model", "m1"}, {"status", "ok"}};
  span.payload = "hi";
  return span;
}

// Encodes then decodes, asserting success.
Span RoundTrip(const Span& span) {
  Result<Span> decoded = DecodeSpan(span.key, EncodeValue(span));
  EXPECT_TRUE(decoded.ok()) << decoded.status().ToString();
  return decoded.ok() ? std::move(decoded).value() : Span{};
}

// The start of a value (version, parent_id, duration, name), for building
// malformed values by hand.
std::string ValuePrefix() {
  std::string v;
  v.push_back(static_cast<char>(kSpanFormatVersion));
  PutFixed64(&v, 0);
  PutVarint64(&v, 0);
  PutLengthPrefixed(&v, "name");
  return v;
}

void ExpectCorruption(std::string_view value, const char* why) {
  Result<Span> decoded = DecodeSpan(SpanKey{}, value);
  ASSERT_FALSE(decoded.ok()) << why;
  EXPECT_TRUE(decoded.status().IsCorruption())
      << why << ": " << decoded.status().ToString();
}

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------

// Pins the on-disk key format. If this fails, existing files would no longer
// decode correctly.
TEST(SpanKeyCodecTest, GoldenBytes) {
  EXPECT_EQ(EncodeKey(ExampleSpan().key),
            Bytes({
                0x18, 0xdb, 0xc7, 0xa7, 0xb9, 0xc4, 0x00, 0x00,  // timestamp
                0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3, 0x4d, 0xa6,  // trace_id
                0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36,  //
                0x00, 0xf0, 0x67, 0xaa, 0x0b, 0xa9, 0x02, 0xb7,  // span_id
            }));
}

TEST(SpanKeyCodecTest, RoundTrip) {
  SpanKey max_key;
  max_key.timestamp_ns = kMax64;
  max_key.trace_id.fill(0xff);
  max_key.span_id = kMax64;

  for (const SpanKey& key : {SpanKey{}, ExampleSpan().key, max_key}) {
    std::string encoded = EncodeKey(key);
    ASSERT_EQ(encoded.size(), kKeySize);
    Result<SpanKey> decoded = DecodeKey(encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    EXPECT_EQ(*decoded, key);
  }
}

TEST(SpanKeyCodecTest, DecodeRejectsWrongSize) {
  std::string key = EncodeKey(ExampleSpan().key);
  for (size_t size : {size_t{0}, size_t{1}, kKeySize - 1, kKeySize + 1}) {
    std::string bytes = key;
    bytes.resize(size, '\0');
    Result<SpanKey> decoded = DecodeKey(bytes);
    ASSERT_FALSE(decoded.ok()) << "size=" << size;
    EXPECT_TRUE(decoded.status().IsCorruption()) << "size=" << size;
  }
}

// The property the whole storage engine relies on: comparing encoded keys as
// bytes gives the same order as comparing SpanKeys.
TEST(SpanKeyCodecTest, ByteOrderMatchesSpanKeyOrder) {
  std::mt19937_64 rng(/*seed=*/42);
  auto random_key = [&rng] {
    SpanKey k;
    k.timestamp_ns = rng();
    for (uint8_t& b : k.trace_id) b = static_cast<uint8_t>(rng());
    k.span_id = rng();
    return k;
  };

  for (int i = 0; i < 10000; ++i) {
    SpanKey a = random_key();
    SpanKey b = random_key();
    // Make the keys share a prefix so that every field and byte position gets
    // to decide the comparison, not just the first byte of the timestamp.
    switch (i % 4) {
      case 0:  // completely random
        break;
      case 1:  // timestamps differ only in the low byte
        b.timestamp_ns = (a.timestamp_ns & ~uint64_t{0xff}) | (rng() & 0xff);
        break;
      case 2:  // same timestamp, so trace_id decides
        b.timestamp_ns = a.timestamp_ns;
        break;
      case 3:  // same timestamp and trace_id, so span_id decides
        b.timestamp_ns = a.timestamp_ns;
        b.trace_id = a.trace_id;
        break;
    }

    std::string ea = EncodeKey(a);
    std::string eb = EncodeKey(b);
    EXPECT_EQ(a < b, ea < eb) << "iteration " << i;
    EXPECT_EQ(a == b, ea == eb) << "iteration " << i;
  }
}

// ---------------------------------------------------------------------------
// Value: encoding
// ---------------------------------------------------------------------------

// Pins the on-disk value format. Round-trip tests cannot catch a format change
// made to both the encoder and decoder at once; this test can.
TEST(SpanValueCodecTest, GoldenBytes) {
  EXPECT_EQ(EncodeValue(ExampleSpan()),
            Bytes({
                0x01,                                            // version
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // parent_id
                0x80, 0xf9, 0x85, 0xd4, 0x04,                    // duration
                0x08, 'l', 'l', 'm', '_', 'c', 'a', 'l', 'l',    // name
                0x02,                                            // tag count
                0x05, 'm', 'o', 'd', 'e', 'l',                   // tag key
                0x02, 'm', '1',                                  // tag value
                0x06, 's', 't', 'a', 't', 'u', 's',              // tag key
                0x02, 'o', 'k',                                  // tag value
                0x02, 'h', 'i',                                  // payload
            }));
}

// The same tags inserted in a different order must encode identically. This
// is why tags are a std::map rather than an unordered_map.
TEST(SpanValueCodecTest, EncodingIsDeterministic) {
  Span a = ExampleSpan();
  a.tags.clear();
  a.tags["status"] = "ok";
  a.tags["model"] = "m1";
  a.tags["agent"] = "planner";

  Span b = ExampleSpan();
  b.tags.clear();
  b.tags["agent"] = "planner";
  b.tags["model"] = "m1";
  b.tags["status"] = "ok";

  EXPECT_EQ(EncodeValue(a), EncodeValue(b));
}

// ---------------------------------------------------------------------------
// Value: round trips
// ---------------------------------------------------------------------------

TEST(SpanValueCodecTest, RoundTripExample) {
  Span span = ExampleSpan();
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, RoundTripEmptyFields) {
  Span span;  // empty name, no tags, empty payload, all numbers 0
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, RoundTripMaxValues) {
  Span span = ExampleSpan();
  span.key.timestamp_ns = kMax64;
  span.key.span_id = kMax64;
  span.parent_id = kMax64;
  span.duration_ns = kMax64;  // 10-byte varint
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, RoundTripLargePayload) {
  Span span = ExampleSpan();
  span.payload = std::string(20 * 1024, 'p');
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, RoundTripBinaryData) {
  Span span = ExampleSpan();
  span.payload = std::string("\x00\x01\xff\x00\x80", 5);
  span.name = std::string("a\0b", 3);
  span.tags = {{"", ""}, {std::string("k\0", 2), std::string("\xff", 1)}};
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, RoundTripManyTags) {
  Span span = ExampleSpan();
  for (int i = 0; i < 200; ++i) {
    span.tags["tag" + std::to_string(i)] = "value" + std::to_string(i);
  }
  EXPECT_EQ(RoundTrip(span), span);
}

TEST(SpanValueCodecTest, DecodeUsesTheGivenKey) {
  Span span = ExampleSpan();
  SpanKey other_key = span.key;
  other_key.span_id = 7;
  Result<Span> decoded = DecodeSpan(other_key, EncodeValue(span));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->key, other_key);
}

// The decoded Span must own its data: destroying the input buffer afterwards
// must not affect it. If DecodeSpan kept views into the buffer, ASan would
// report a use-after-free here.
TEST(SpanValueCodecTest, DecodedSpanOwnsItsData) {
  Span span = ExampleSpan();
  span.payload = std::string(1000, 'x');  // long enough to live on the heap

  Result<Span> decoded = Status::Corruption("not decoded yet");
  {
    std::string buffer = EncodeValue(span);
    decoded = DecodeSpan(span.key, buffer);
    buffer.assign(buffer.size(), '\0');  // overwrite the bytes...
  }                                      // ...then free them
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, span);
}

// ---------------------------------------------------------------------------
// Value: corruption
// ---------------------------------------------------------------------------

// Cutting a valid value short at any byte must fail cleanly. Under ASan this
// also proves no field reads past the end of its input.
TEST(SpanValueCodecTest, EveryTruncationIsCorruption) {
  Span span = ExampleSpan();
  span.duration_ns = kMax64;  // exercise a 10-byte varint too
  std::string value = EncodeValue(span);
  for (size_t len = 0; len < value.size(); ++len) {
    // Copy the prefix into its own buffer so ASan catches any over-read.
    std::string prefix = value.substr(0, len);
    Result<Span> decoded = DecodeSpan(span.key, prefix);
    ASSERT_FALSE(decoded.ok()) << "len=" << len;
    EXPECT_TRUE(decoded.status().IsCorruption()) << "len=" << len;
  }
}

TEST(SpanValueCodecTest, RejectsEmptyValue) {
  ExpectCorruption("", "empty value");
}

TEST(SpanValueCodecTest, RejectsUnknownVersion) {
  for (uint8_t version : {uint8_t{0x00}, uint8_t{0x02}, uint8_t{0xff}}) {
    std::string value = EncodeValue(ExampleSpan());
    value[0] = static_cast<char>(version);
    ExpectCorruption(value, "unknown version");
  }
}

TEST(SpanValueCodecTest, RejectsTrailingBytes) {
  ExpectCorruption(EncodeValue(ExampleSpan()) + '\x00', "one trailing byte");
}

TEST(SpanValueCodecTest, RejectsTagsOutOfOrder) {
  std::string value = ValuePrefix();
  PutVarint64(&value, 2);
  PutLengthPrefixed(&value, "status");  // "status" > "model": wrong order
  PutLengthPrefixed(&value, "ok");
  PutLengthPrefixed(&value, "model");
  PutLengthPrefixed(&value, "m1");
  PutLengthPrefixed(&value, "payload");
  ExpectCorruption(value, "tags out of order");
}

TEST(SpanValueCodecTest, RejectsDuplicateTags) {
  std::string value = ValuePrefix();
  PutVarint64(&value, 2);
  PutLengthPrefixed(&value, "model");
  PutLengthPrefixed(&value, "m1");
  PutLengthPrefixed(&value, "model");
  PutLengthPrefixed(&value, "m2");
  PutLengthPrefixed(&value, "payload");
  ExpectCorruption(value, "duplicate tag");
}

TEST(SpanValueCodecTest, RejectsNameLongerThanValue) {
  std::string value;
  value.push_back(static_cast<char>(kSpanFormatVersion));
  PutFixed64(&value, 0);
  PutVarint64(&value, 0);
  PutVarint64(&value, 1000);  // name length 1000, but only 3 bytes follow
  value += "abc";
  ExpectCorruption(value, "name length too long");
}

// A corrupted tag count in the billions must fail by running out of input,
// not by trying to allocate space for billions of tags.
TEST(SpanValueCodecTest, RejectsHugeTagCount) {
  std::string value = ValuePrefix();
  PutVarint64(&value, uint64_t{1} << 40);
  PutLengthPrefixed(&value, "model");
  PutLengthPrefixed(&value, "m1");
  ExpectCorruption(value, "huge tag count");
}

}  // namespace
}  // namespace tracedb
