#ifndef TRACEDB_UTIL_CODING_H_
#define TRACEDB_UTIL_CODING_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Low-level helpers for turning integers and byte strings into bytes and back.
//
// Naming convention:
//   Encode... / Decode...  work on a raw buffer whose size the caller has
//                          already checked.
//   Put...                 append the encoding to a std::string.
//   Get...                 read from the front of a std::string_view and
//                          advance it past what was read. Returns false if the
//                          input is too short or malformed, in which case the
//                          input is left unchanged.
//
// Every Get... function treats its input as untrusted: it never reads past the
// end of the view, whatever the bytes contain.

namespace tracedb {

// A varint stores 7 bits per byte, so a 64-bit value takes at most
// ceil(64 / 7) = 10 bytes.
inline constexpr size_t kMaxVarint64Length = 10;

// ---- Fixed-width, little-endian ----
// Used for fields that are never compared as bytes: parent_id, and later
// lengths, checksums and file offsets.

// Writes exactly 4 / 8 bytes to dst.
void EncodeFixed32(char* dst, uint32_t value);
void EncodeFixed64(char* dst, uint64_t value);

// Reads exactly 4 / 8 bytes from ptr.
uint32_t DecodeFixed32(const char* ptr);
uint64_t DecodeFixed64(const char* ptr);

// Appends value to the end of *dst as exactly 4 / 8 little-endian bytes.
// Example: PutFixed32(&s, 300) appends 2c 01 00 00.
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

// Reads 4 / 8 little-endian bytes from the front of *input into *value, then
// advances *input past them. Returns false (and changes nothing) if fewer than
// 4 / 8 bytes remain.
// Example: input = [2c 01 00 00 ff] -> *value = 300, input = [ff].
bool GetFixed32(std::string_view* input, uint32_t* value);
bool GetFixed64(std::string_view* input, uint64_t* value);

// ---- Fixed-width, big-endian ----
// Used for key fields. Most significant byte first, so comparing the encoded
// bytes (memcmp) gives the same order as comparing the numbers.

// Writes exactly 8 bytes to dst / reads exactly 8 bytes from ptr.
void EncodeBigEndian64(char* dst, uint64_t value);
uint64_t DecodeBigEndian64(const char* ptr);

// Appends value to the end of *dst as exactly 8 big-endian bytes.
// Example: PutBigEndian64(&s, 0x0102030405060708) appends 01 02 03 ... 08.
void PutBigEndian64(std::string* dst, uint64_t value);

// ---- Varints ----
// 7 bits of the value per byte, lowest bits first. The top bit of each byte is
// set when more bytes follow. Small values take fewer bytes: 0-127 take 1,
// 128-16383 take 2, ..., UINT64_MAX takes 10.

// Appends value to the end of *dst as a varint (1-10 bytes).
// Example: PutVarint64(&s, 300) appends ac 02.
void PutVarint64(std::string* dst, uint64_t value);

// Reads a varint from the front of *input into *value, then advances *input
// past it. Returns false (and changes nothing) on failure.
// Example: input = [ac 02 ff] -> *value = 300, input = [ff].
//
// Fails on truncated input, on more than kMaxVarint64Length bytes, and on a
// 10th byte that would overflow 64 bits.
bool GetVarint64(std::string_view* input, uint64_t* value);

// Number of bytes PutVarint64 would write for value.
size_t VarintLength(uint64_t value);

// ---- Length-prefixed byte strings ----
// A varint length followed by that many bytes. Any bytes are allowed,
// including 0x00.

// Appends the length of value as a varint, then the bytes of value.
// Example: PutLengthPrefixed(&s, "hi") appends 02 68 69.
void PutLengthPrefixed(std::string* dst, std::string_view value);

// Reads a varint length and then that many bytes from the front of *input,
// sets *result to those bytes, and advances *input past both.
// Example: input = [02 68 69 ff] -> *result = "hi", input = [ff].
//
// On success, *result is a view into the input's underlying buffer (no copy),
// so it is only valid while that buffer is alive. Fails if the length is
// malformed or claims more bytes than remain.
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

}  // namespace tracedb

#endif  // TRACEDB_UTIL_CODING_H_
