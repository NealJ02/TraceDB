#ifndef TRACEDB_SPAN_H_
#define TRACEDB_SPAN_H_

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <string>

namespace tracedb {

// A 128-bit trace ID, stored as 16 raw bytes. C++ has no 128-bit integer type,
// and the ID is only ever compared, never used in arithmetic.
using TraceId = std::array<uint8_t, 16>;

// Uniquely identifies a span, and determines the order spans are stored in.
//
// Spans sort by timestamp first, so all spans in a time range are adjacent.
// trace_id and span_id break ties between spans that start at the same
// nanosecond.
struct SpanKey {
  uint64_t timestamp_ns = 0;  // span start time, nanoseconds since the epoch
  TraceId trace_id{};         // which trace (request) this span belongs to
  uint64_t span_id = 0;       // unique within the trace

  // Compares the fields in declaration order: timestamp_ns, then trace_id
  // (byte by byte), then span_id. The encoded key's bytes must sort in exactly
  // this order; span_codec_test checks that they do.
  auto operator<=>(const SpanKey&) const = default;
  bool operator==(const SpanKey&) const = default;
};

// One timed operation within a trace, such as a single LLM call or tool call.
struct Span {
  SpanKey key;

  uint64_t parent_id = 0;    // span_id of the calling span; 0 for the root
  std::string name;          // operation name, e.g. "llm_call"
  uint64_t duration_ns = 0;  // how long the operation took

  // Labels used for filtering, e.g. {"model": "m1", "status": "ok"}.
  // A std::map keeps keys sorted, so the same span always encodes to the same
  // bytes.
  std::map<std::string, std::string> tags;

  // The operation's content (prompts, responses, tool output). Arbitrary
  // bytes: TraceDB stores them unchanged and never looks inside.
  std::string payload;

  bool operator==(const Span&) const = default;
};

}  // namespace tracedb

#endif  // TRACEDB_SPAN_H_
