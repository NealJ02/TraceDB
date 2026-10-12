// Differential (model-based) test.
//
// Runs thousands of random operations against both TraceDB and a reference
// model -- a std::map<SpanKey, Span>, which is obviously correct -- and checks
// after every operation that the two agree. Any disagreement is a bug in
// TraceDB.
//
// Every run is driven by a seeded random number generator, so a failure can be
// replayed exactly:
//
//   TRACEDB_SEED=<seed> ./build/tracedb_tests --gtest_filter='Differential*'
//
// Each later feature (reopen, flush, scan, compaction, retention) adds its
// operations to the Op enum below, so it is checked against the model too.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tracedb/db.h"

namespace tracedb {
namespace {

constexpr uint64_t kNumSeeds = 10;
constexpr int kOpsPerSeed = 5000;
constexpr uint64_t kMax64 = std::numeric_limits<uint64_t>::max();

using Model = std::map<SpanKey, Span>;

std::string DescribeKey(const SpanKey& key) {
  std::ostringstream out;
  out << "{timestamp_ns=" << key.timestamp_ns << " trace_id=";
  for (uint8_t b : key.trace_id) {
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  out << std::dec << " span_id=" << key.span_id << "}";
  return out.str();
}

// Produces random keys and spans from a seed.
//
// Keys are drawn from small pools (52 timestamps x 4 trace IDs x 4 span IDs =
// 832 keys), so random operations frequently overwrite existing keys and look
// up keys that exist. With fully random keys, every Put would create a new key
// and almost every Get would miss, and the test would exercise very little.
//
// Only std::mt19937_64's raw output (which the C++ standard fully specifies)
// and % are used, so a seed produces the same operations on every platform.
// (std::uniform_int_distribution can differ between standard libraries.)
class SpanGenerator {
 public:
  explicit SpanGenerator(uint64_t seed) : rng_(seed) {
    const uint64_t base = 1791244800000000000ULL;  // 2026-10-06 00:00 UTC
    for (uint64_t i = 0; i < 50; ++i) timestamps_.push_back(base + i * 1000);
    timestamps_.push_back(0);       // extremes of the range
    timestamps_.push_back(kMax64);

    TraceId t{};
    trace_ids_.push_back(t);  // all 0x00
    t.fill(0xff);
    trace_ids_.push_back(t);  // all 0xff
    for (uint8_t i = 0; i < 16; ++i) t[i] = i;
    trace_ids_.push_back(t);  // 00 01 02 ... 0f
    t[15] = 0x10;
    trace_ids_.push_back(t);  // differs from the previous only in the last byte

    span_ids_ = {0, 1, 2, kMax64};
  }

  // A random number in [0, n).
  uint64_t Uniform(uint64_t n) { return rng_() % n; }

  // True with probability 1/n.
  bool OneIn(uint64_t n) { return Uniform(n) == 0; }

  SpanKey RandomKey() {
    SpanKey key;
    key.timestamp_ns = timestamps_[Uniform(timestamps_.size())];
    key.trace_id = trace_ids_[Uniform(trace_ids_.size())];
    key.span_id = span_ids_[Uniform(span_ids_.size())];
    return key;
  }

  Span RandomSpan() {
    static const char* const kNames[] = {"llm_call", "tool_call", "retrieval",
                                         "handle_request"};
    static const char* const kTagKeys[] = {"agent", "model", "status", "tool"};
    static const char* const kTagValues[] = {"planner", "m1", "ok", "error",
                                             ""};

    Span span;
    span.key = RandomKey();
    span.parent_id = OneIn(4) ? 0 : rng_();
    span.name = kNames[Uniform(std::size(kNames))];
    span.duration_ns = OneIn(50) ? kMax64 : Uniform(10'000'000'000ULL);

    // 0-3 tags. A repeated tag key just overwrites, giving fewer tags.
    const uint64_t num_tags = Uniform(4);
    for (uint64_t i = 0; i < num_tags; ++i) {
      span.tags[kTagKeys[Uniform(std::size(kTagKeys))]] =
          kTagValues[Uniform(std::size(kTagValues))];
    }

    // Payload sizes: mostly small, sometimes medium, occasionally 20 KiB.
    // Random bytes, so every byte value (including 0x00) appears.
    const uint64_t r = Uniform(100);
    size_t size;
    if (r < 70) {
      size = Uniform(201);
    } else if (r < 95) {
      size = 200 + Uniform(3801);
    } else {
      size = 20 * 1024;
    }
    span.payload.reserve(size);
    for (size_t i = 0; i < size; ++i) {
      span.payload.push_back(static_cast<char>(rng_()));
    }
    return span;
  }

  // Every key the generator can produce.
  std::vector<SpanKey> AllKeys() const {
    std::vector<SpanKey> keys;
    for (uint64_t ts : timestamps_) {
      for (const TraceId& trace_id : trace_ids_) {
        for (uint64_t span_id : span_ids_) {
          keys.push_back(SpanKey{ts, trace_id, span_id});
        }
      }
    }
    return keys;
  }

 private:
  std::mt19937_64 rng_;
  std::vector<uint64_t> timestamps_;
  std::vector<TraceId> trace_ids_;
  std::vector<uint64_t> span_ids_;
};

// The operations the test can perform. Later steps add more here (kReopen,
// kFlush, kScan, kCompactAll, kDropBefore).
enum class Op {
  kPut,            // write a random span
  kGetWrittenKey,  // read a key that has been written
  kGetRandomKey,   // read any key from the pools (may or may not exist)
};

Op PickOp(SpanGenerator& gen, bool nothing_written_yet) {
  const uint64_t r = gen.Uniform(100);
  if (r < 50) return Op::kPut;
  if (r < 80 && !nothing_written_yet) return Op::kGetWrittenKey;
  return Op::kGetRandomKey;
}

// Checks that db->Get(key) returns exactly what the model says it should.
::testing::AssertionResult GetMatchesModel(TraceDB& db, const Model& model,
                                           const SpanKey& key) {
  Result<std::optional<Span>> got = db.Get(key);
  if (!got.ok()) {
    return ::testing::AssertionFailure()
           << "Get" << DescribeKey(key) << " failed: " << got.status().ToString();
  }

  auto it = model.find(key);
  if (it == model.end()) {
    if (got->has_value()) {
      return ::testing::AssertionFailure()
             << "Get" << DescribeKey(key)
             << " returned a span, but the model has none";
    }
    return ::testing::AssertionSuccess();
  }

  if (!got->has_value()) {
    return ::testing::AssertionFailure()
           << "Get" << DescribeKey(key)
           << " returned nothing, but the model has a span";
  }
  if (**got != it->second) {
    return ::testing::AssertionFailure()
           << "Get" << DescribeKey(key)
           << " returned a different span than the model (e.g. payload size "
           << (*got)->payload.size() << " vs " << it->second.payload.size()
           << ")";
  }
  return ::testing::AssertionSuccess();
}

// Runs one random sequence of `num_ops` operations from `seed`. Stops at the
// first disagreement.
void RunDifferential(uint64_t seed, int num_ops) {
  SCOPED_TRACE("seed " + std::to_string(seed) +
               " (replay with TRACEDB_SEED=" + std::to_string(seed) + ")");

  Result<std::unique_ptr<TraceDB>> opened =
      TraceDB::Open("differential_db", Options{});
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  std::unique_ptr<TraceDB> db = std::move(opened).value();

  Model model;
  std::vector<SpanKey> written;  // every distinct key written so far
  SpanGenerator gen(seed);

  // Counted to check that the generator really produces overwrites, hits and
  // misses; otherwise the test would quietly stop testing them.
  int overwrites = 0;
  int random_hits = 0;
  int random_misses = 0;

  for (int i = 0; i < num_ops; ++i) {
    switch (PickOp(gen, written.empty())) {
      case Op::kPut: {
        Span span = gen.RandomSpan();
        Status s = db->Put(span);
        ASSERT_TRUE(s.ok()) << "op " << i << ": Put" << DescribeKey(span.key)
                            << " failed: " << s.ToString();
        const bool is_new = model.insert_or_assign(span.key, span).second;
        if (is_new) {
          written.push_back(span.key);
        } else {
          ++overwrites;
        }
        break;
      }
      case Op::kGetWrittenKey: {
        const SpanKey& key = written[gen.Uniform(written.size())];
        ASSERT_TRUE(GetMatchesModel(*db, model, key)) << "op " << i;
        break;
      }
      case Op::kGetRandomKey: {
        const SpanKey key = gen.RandomKey();
        if (model.contains(key)) {
          ++random_hits;
        } else {
          ++random_misses;
        }
        ASSERT_TRUE(GetMatchesModel(*db, model, key)) << "op " << i;
        break;
      }
    }
  }

  // Final sweep: every possible key, present or not, must match the model.
  for (const SpanKey& key : gen.AllKeys()) {
    ASSERT_TRUE(GetMatchesModel(*db, model, key)) << "final sweep";
  }

  EXPECT_GT(overwrites, 0) << "the generator produced no overwrites";
  EXPECT_GT(random_hits, 0) << "random Gets never found an existing key";
  EXPECT_GT(random_misses, 0) << "random Gets never missed";
}

TEST(DifferentialTest, PutGet) {
  // TRACEDB_SEED=<n> replays a single seed, e.g. one that failed.
  if (const char* env = std::getenv("TRACEDB_SEED")) {
    RunDifferential(std::strtoull(env, nullptr, 10), kOpsPerSeed);
    return;
  }
  for (uint64_t seed = 1; seed <= kNumSeeds; ++seed) {
    RunDifferential(seed, kOpsPerSeed);
    if (HasFatalFailure()) return;  // stop at the first failing seed
  }
}

}  // namespace
}  // namespace tracedb
