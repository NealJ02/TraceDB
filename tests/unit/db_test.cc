// Tests for the database through its public API only (tracedb/db.h), the way
// a user of the library would call it.

#include "tracedb/db.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace tracedb {
namespace {

Span MakeSpan(uint64_t timestamp_ns, uint64_t span_id,
              std::string payload = "payload") {
  Span span;
  span.key.timestamp_ns = timestamp_ns;
  span.key.trace_id = {0x4b, 0xf9, 0x2f, 0x35, 0x77, 0xb3, 0x4d, 0xa6,
                       0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36};
  span.key.span_id = span_id;
  span.parent_id = 1;
  span.name = "llm_call";
  span.duration_ns = 1250000000;
  span.tags = {{"model", "m1"}, {"status", "ok"}};
  span.payload = std::move(payload);
  return span;
}

// A test fixture: SetUp runs before each TEST_F, giving every test a freshly
// opened database in db_.
class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<TraceDB>> opened = TraceDB::Open("testdb", Options{});
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    db_ = std::move(opened).value();
  }

  // Gets `key` and asserts the lookup itself succeeded.
  std::optional<Span> GetOk(const SpanKey& key) {
    Result<std::optional<Span>> r = db_->Get(key);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? std::move(r).value() : std::nullopt;
  }

  std::unique_ptr<TraceDB> db_;
};

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

TEST(DBOpenTest, RejectsEmptyDir) {
  Result<std::unique_ptr<TraceDB>> opened = TraceDB::Open("", Options{});
  ASSERT_FALSE(opened.ok());
  EXPECT_TRUE(opened.status().IsInvalidArgument());
}

TEST(DBOpenTest, RejectsZeroMemtableSize) {
  Options options;
  options.memtable_size_bytes = 0;
  Result<std::unique_ptr<TraceDB>> opened = TraceDB::Open("testdb", options);
  ASSERT_FALSE(opened.ok());
  EXPECT_TRUE(opened.status().IsInvalidArgument());
}

TEST(DBOpenTest, AcceptsCustomMemtableSize) {
  Options options;
  options.memtable_size_bytes = 1;
  EXPECT_TRUE(TraceDB::Open("testdb", options).ok());
}

// ---------------------------------------------------------------------------
// Put / Get
// ---------------------------------------------------------------------------

TEST_F(DBTest, PutThenGet) {
  Span span = MakeSpan(100, 1);
  ASSERT_TRUE(db_->Put(span).ok());
  EXPECT_EQ(GetOk(span.key), span);
}

// A missing key is not an error: the result is OK and holds nullopt.
TEST_F(DBTest, GetMissingKeyIsOkAndEmpty) {
  Result<std::optional<Span>> r = db_->Get(MakeSpan(100, 1).key);
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r->has_value());

  ASSERT_TRUE(db_->Put(MakeSpan(100, 1)).ok());
  EXPECT_FALSE(GetOk(MakeSpan(100, 2).key).has_value());
}

TEST_F(DBTest, OverwriteReturnsLatest) {
  Span first = MakeSpan(100, 1, "first");
  Span second = MakeSpan(100, 1, "second");  // same key
  ASSERT_TRUE(db_->Put(first).ok());
  ASSERT_TRUE(db_->Put(second).ok());
  EXPECT_EQ(GetOk(first.key), second);

  Span third = MakeSpan(100, 1, "third");
  ASSERT_TRUE(db_->Put(third).ok());
  EXPECT_EQ(GetOk(first.key), third);
}

// Keys that differ in a single field (or a single byte) are separate spans.
TEST_F(DBTest, KeysDifferingInOneFieldAreSeparate) {
  Span base = MakeSpan(100, 1, "base");

  Span other_time = base;
  other_time.key.timestamp_ns = 101;
  other_time.payload = "other_time";

  Span other_trace = base;
  other_trace.key.trace_id[15] ^= 0x01;  // flip one bit of the last byte
  other_trace.payload = "other_trace";

  Span other_span_id = base;
  other_span_id.key.span_id = 2;
  other_span_id.payload = "other_span_id";

  for (const Span& s : {base, other_time, other_trace, other_span_id}) {
    ASSERT_TRUE(db_->Put(s).ok());
  }
  for (const Span& s : {base, other_time, other_trace, other_span_id}) {
    EXPECT_EQ(GetOk(s.key), s);
  }
}

TEST_F(DBTest, ManySpans) {
  for (uint64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db_->Put(MakeSpan(i, i, "payload" + std::to_string(i))).ok());
  }
  for (uint64_t i = 0; i < 1000; ++i) {
    Span expected = MakeSpan(i, i, "payload" + std::to_string(i));
    EXPECT_EQ(GetOk(expected.key), expected) << "i=" << i;
  }
}

TEST_F(DBTest, LargeAndBinaryPayloads) {
  Span large = MakeSpan(100, 1, std::string(20 * 1024, 'x'));
  Span binary = MakeSpan(100, 2, std::string("\x00\xff\x00\x80", 4));
  Span empty = MakeSpan(100, 3, "");
  Span bare;  // every field at its default, including the all-zero key
  for (const Span& s : {large, binary, empty, bare}) {
    ASSERT_TRUE(db_->Put(s).ok());
  }
  for (const Span& s : {large, binary, empty, bare}) {
    EXPECT_EQ(GetOk(s.key), s);
  }
}

// The database keeps its own copy: changing the caller's Span after Put, or
// the Span returned by Get, must not change what is stored.
TEST_F(DBTest, StoredDataIsIndependentOfCallerCopies) {
  Span span = MakeSpan(100, 1, "original");
  ASSERT_TRUE(db_->Put(span).ok());
  span.payload = "changed after put";

  std::optional<Span> got = GetOk(span.key);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->payload, "original");
  got->payload = "changed after get";

  EXPECT_EQ(GetOk(span.key)->payload, "original");
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

TEST_F(DBTest, CallsAfterCloseFail) {
  Span span = MakeSpan(100, 1);
  ASSERT_TRUE(db_->Put(span).ok());
  ASSERT_TRUE(db_->Close().ok());

  Status put = db_->Put(span);
  EXPECT_TRUE(put.IsInvalidArgument()) << put.ToString();

  Result<std::optional<Span>> got = db_->Get(span.key);
  ASSERT_FALSE(got.ok());
  EXPECT_TRUE(got.status().IsInvalidArgument());
}

TEST_F(DBTest, CloseTwiceIsOk) {
  EXPECT_TRUE(db_->Close().ok());
  EXPECT_TRUE(db_->Close().ok());
}

// Destroying an open database without calling Close must clean up fully.
// (ASan's leak checker reports any memory left behind.)
TEST_F(DBTest, DestroyWithoutClose) {
  ASSERT_TRUE(db_->Put(MakeSpan(100, 1)).ok());
  db_.reset();  // destroys the database now
  EXPECT_EQ(db_, nullptr);
}

}  // namespace
}  // namespace tracedb
