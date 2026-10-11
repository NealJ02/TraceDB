#ifndef TRACEDB_SPAN_CODEC_H_
#define TRACEDB_SPAN_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tracedb/span.h"
#include "tracedb/status.h"

// Converts a Span to and from the bytes stored on disk.
//
// A span is stored as two byte strings:
//
//   key   (32 bytes, fixed)
//     timestamp_ns  8 bytes, big-endian
//     trace_id     16 bytes, as-is
//     span_id       8 bytes, big-endian
//   Big-endian puts the most significant byte first, so comparing two keys'
//   bytes gives the same order as comparing the SpanKeys.
//
//   value (variable length)
//     version      1 byte (kSpanFormatVersion)
//     parent_id    fixed64, little-endian
//     duration_ns  varint64
//     name         length-prefixed
//     tag_count    varint64
//       tag key    length-prefixed   } repeated tag_count times,
//       tag value  length-prefixed   } in strictly increasing key order
//     payload      length-prefixed
//
// Changing this layout changes the on-disk format: existing files would no
// longer decode. Bump kSpanFormatVersion instead.

namespace tracedb {

inline constexpr size_t kKeySize = 32;
inline constexpr uint8_t kSpanFormatVersion = 1;

// Returns exactly kKeySize bytes.
std::string EncodeKey(const SpanKey& key);

// Fails with Corruption unless bytes is exactly kKeySize long.
Result<SpanKey> DecodeKey(std::string_view bytes);

// Encodes every field of span except its key (which is stored separately).
std::string EncodeValue(const Span& span);

// Rebuilds a Span from its key and encoded value. The returned Span owns all
// of its data, so value's buffer may be freed afterwards.
//
// Fails with Corruption if value is truncated, has an unknown version, has
// tags out of order or duplicated, or has bytes left over after the payload.
Result<Span> DecodeSpan(const SpanKey& key, std::string_view value);

}  // namespace tracedb

#endif  // TRACEDB_SPAN_CODEC_H_
