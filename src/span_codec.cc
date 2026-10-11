#include "span_codec.h"

#include <cstring>

#include "util/coding.h"

namespace tracedb {

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------

std::string EncodeKey(const SpanKey& key) {
  std::string out;
  out.reserve(kKeySize);  // allocate once; the size is always the same

  PutBigEndian64(&out, key.timestamp_ns);  // bytes 0-7
  // trace_id is already raw bytes, so it is copied as-is. The cast only
  // changes the pointer type (uint8_t* -> char*) to match std::string.
  out.append(reinterpret_cast<const char*>(key.trace_id.data()),
             key.trace_id.size());         // bytes 8-23
  PutBigEndian64(&out, key.span_id);       // bytes 24-31

  return out;
}

Result<SpanKey> DecodeKey(std::string_view bytes) {
  // The key has a fixed layout, so one size check makes every read below safe.
  if (bytes.size() != kKeySize) {
    return Status::Corruption("span key has " + std::to_string(bytes.size()) +
                              " bytes, expected " + std::to_string(kKeySize));
  }

  SpanKey key;
  key.timestamp_ns = DecodeBigEndian64(bytes.data());
  std::memcpy(key.trace_id.data(), bytes.data() + 8, key.trace_id.size());
  key.span_id = DecodeBigEndian64(bytes.data() + 24);
  return key;
}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

std::string EncodeValue(const Span& span) {
  std::string out;

  out.push_back(static_cast<char>(kSpanFormatVersion));
  PutFixed64(&out, span.parent_id);
  PutVarint64(&out, span.duration_ns);
  PutLengthPrefixed(&out, span.name);

  PutVarint64(&out, span.tags.size());
  for (const auto& [tag_key, tag_value] : span.tags) {  // sorted by tag_key
    PutLengthPrefixed(&out, tag_key);
    PutLengthPrefixed(&out, tag_value);
  }

  PutLengthPrefixed(&out, span.payload);
  return out;
}

Result<Span> DecodeSpan(const SpanKey& key, std::string_view value) {
  // `value` is consumed from the front as each field is read. Every Get...
  // call checks bounds itself, so a truncated or garbled value fails with
  // Corruption instead of reading past the end.
  Span span;
  span.key = key;

  // Version byte.
  if (value.empty()) return Status::Corruption("span value is empty");
  uint8_t version = static_cast<uint8_t>(value[0]);
  value.remove_prefix(1);
  if (version != kSpanFormatVersion) {
    return Status::Corruption("unknown span format version " +
                              std::to_string(version));
  }

  // Fixed-size and varint fields.
  if (!GetFixed64(&value, &span.parent_id)) {
    return Status::Corruption("bad span parent_id");
  }
  if (!GetVarint64(&value, &span.duration_ns)) {
    return Status::Corruption("bad span duration_ns");
  }

  // Name. GetLengthPrefixed returns a view into `value`'s buffer, so copy it
  // into the Span, which must own its data.
  std::string_view name;
  if (!GetLengthPrefixed(&value, &name)) {
    return Status::Corruption("bad span name");
  }
  span.name.assign(name);

  // Tags.
  uint64_t tag_count;
  if (!GetVarint64(&value, &tag_count)) {
    return Status::Corruption("bad span tag count");
  }
  // Do not reserve space based on tag_count: a corrupted count could be in the
  // billions. Each iteration reads real bytes, so a bad count simply runs out
  // of input and fails.
  for (uint64_t i = 0; i < tag_count; ++i) {
    std::string_view tag_key;
    std::string_view tag_value;
    if (!GetLengthPrefixed(&value, &tag_key) ||
        !GetLengthPrefixed(&value, &tag_value)) {
      return Status::Corruption("bad span tag");
    }
    // The encoder writes tags from a std::map: sorted, no duplicates. Anything
    // else cannot come from valid data.
    if (!span.tags.empty() && tag_key <= span.tags.rbegin()->first) {
      return Status::Corruption("span tags out of order or duplicated");
    }
    // Tags arrive in order, so each one goes at the end of the map; the hint
    // lets the map insert there without searching.
    span.tags.emplace_hint(span.tags.end(), tag_key, tag_value);
  }

  // Payload.
  std::string_view payload;
  if (!GetLengthPrefixed(&value, &payload)) {
    return Status::Corruption("bad span payload");
  }
  span.payload.assign(payload);

  // A valid value ends exactly after the payload.
  if (!value.empty()) {
    return Status::Corruption("span value has " + std::to_string(value.size()) +
                              " trailing bytes");
  }

  return span;
}

}  // namespace tracedb
