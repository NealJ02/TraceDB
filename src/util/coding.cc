#include "util/coding.h"

namespace tracedb {

// A note on the casts used throughout this file:
//
// - Buffers are passed around as char*, because that is what std::string and
//   std::string_view use. But char may be signed (on x86, the byte 0xf9 reads
//   as -7), which breaks arithmetic. So we view the same memory as uint8_t*
//   (always 0-255) with reinterpret_cast. Both are byte types, so this is
//   always allowed.
//
// - Before shifting a byte left, we widen it with static_cast<uint32_t> or
//   static_cast<uint64_t>. Otherwise C++ promotes the uint8_t to a signed int,
//   and a shift like 0x80 << 24 overflows it, which is undefined behavior.

// ---------------------------------------------------------------------------
// Fixed-width, little-endian
// ---------------------------------------------------------------------------

void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  // Byte i holds bits [8*i, 8*i + 7] of value: lowest byte first.
  // static_cast<uint8_t> keeps only the low 8 bits of the shifted value.
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    buf[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    buf[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(ptr);
  // Put each byte back at its position and OR them together.
  uint32_t result = 0;
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    result |= static_cast<uint32_t>(buf[i]) << (8 * i);
  }
  return result;
}

uint64_t DecodeFixed64(const char* ptr) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    result |= static_cast<uint64_t>(buf[i]) << (8 * i);
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(uint32_t)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(uint64_t)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

bool GetFixed32(std::string_view* input, uint32_t* value) {
  // Check before reading: never read past the end of the input.
  if (input->size() < sizeof(uint32_t)) return false;
  *value = DecodeFixed32(input->data());
  input->remove_prefix(sizeof(uint32_t));
  return true;
}

bool GetFixed64(std::string_view* input, uint64_t* value) {
  if (input->size() < sizeof(uint64_t)) return false;
  *value = DecodeFixed64(input->data());
  input->remove_prefix(sizeof(uint64_t));
  return true;
}

// ---------------------------------------------------------------------------
// Fixed-width, big-endian
// ---------------------------------------------------------------------------

void EncodeBigEndian64(char* dst, uint64_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  // The mirror image of EncodeFixed64: highest byte first.
  // Byte 0 gets bits 56-63, byte 1 gets bits 48-55, ..., byte 7 gets bits 0-7.
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    buf[i] = static_cast<uint8_t>(value >> (8 * (sizeof(uint64_t) - 1 - i)));
  }
}

uint64_t DecodeBigEndian64(const char* ptr) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(ptr);
  // Read left to right. Each step moves what we have so far up one byte and
  // puts the next byte in the low 8 bits, so the first byte ends up highest.
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    result = (result << 8) | buf[i];
  }
  return result;
}

void PutBigEndian64(std::string* dst, uint64_t value) {
  char buf[sizeof(uint64_t)];
  EncodeBigEndian64(buf, value);
  dst->append(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Varints
// ---------------------------------------------------------------------------

void PutVarint64(std::string* dst, uint64_t value) {
  // While the value needs more than 7 bits, emit its low 7 bits with the
  // continuation bit (0x80) set, then move on to the next 7 bits.
  while (value >= 0x80) {
    dst->push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  // The last byte (value now fits in 7 bits) has the continuation bit clear.
  dst->push_back(static_cast<char>(value));
}

bool GetVarint64(std::string_view* input, uint64_t* value) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(input->data());
  uint64_t result = 0;
  for (size_t i = 0; i < kMaxVarint64Length; ++i) {
    // Ran out of input before the last byte: truncated.
    if (i >= input->size()) return false;

    uint8_t byte = buf[i];

    // 9 bytes carry 63 bits, so the 10th byte may only supply bit 63: it must
    // be 0 or 1 (which also means it cannot have the continuation bit set).
    if (i == kMaxVarint64Length - 1 && byte > 1) return false;

    // Put this byte's 7 data bits at their position: byte i holds
    // bits [7*i, 7*i + 6] of the value.
    result |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);

    // Continuation bit clear: this was the last byte.
    if ((byte & 0x80) == 0) {
      *value = result;
      input->remove_prefix(i + 1);
      return true;
    }
  }
  // 10 bytes, all with the continuation bit set: too long to be valid.
  return false;
}

size_t VarintLength(uint64_t value) {
  // Mirrors PutVarint64: one byte per 7 bits, at least one byte.
  size_t length = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++length;
  }
  return length;
}

// ---------------------------------------------------------------------------
// Length-prefixed byte strings
// ---------------------------------------------------------------------------

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint64(dst, value.size());
  dst->append(value.data(), value.size());
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  // Work on a copy so that *input is left unchanged on failure. Without this,
  // a valid length followed by too few bytes would leave *input already
  // advanced past the length.
  std::string_view in = *input;

  uint64_t length;
  if (!GetVarint64(&in, &length)) return false;

  // Never trust a length read from disk: a corrupted one could claim far more
  // bytes than exist.
  if (length > in.size()) return false;

  *result = in.substr(0, length);  // a view into the same buffer, no copy
  in.remove_prefix(length);
  *input = in;
  return true;
}

}  // namespace tracedb
