#ifndef TRACEDB_MEMTABLE_MEMTABLE_H_
#define TRACEDB_MEMTABLE_MEMTABLE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace tracedb {

// The in-memory table of recent writes, sorted by key.
//
// Keys and values are stored as encoded bytes (EncodeKey / EncodeValue), so
// entries sort by comparing key bytes, exactly as they will on disk. Each key
// holds only its latest value, along with the sequence number of the write
// that produced it.
//
// When the memtable's approximate memory usage reaches
// Options::memtable_size_bytes, it is flushed to an SSTable (once flushing
// exists).
//
// Not thread-safe.
class Memtable {
 public:
  // The result of a successful Get.
  struct LookupResult {
    uint64_t seq;
    // Points into the memtable's own storage (no copy). Valid only until the
    // memtable is next modified or destroyed, so use it right away.
    std::string_view value;
  };

  Memtable() = default;

  // A memtable can hold megabytes of data; copying one by accident would be
  // expensive, and there is never a reason to.
  Memtable(const Memtable&) = delete;
  Memtable& operator=(const Memtable&) = delete;

  // Stores `value` under `key`, replacing any existing entry for `key`.
  // The memtable keeps its own copies of the bytes.
  //
  // `seq` must be greater than the seq of the entry being replaced (sequence
  // numbers only increase); this is checked with an assert.
  void Insert(uint64_t seq, std::string_view key, std::string_view value);

  // Returns the entry stored under `key`, or std::nullopt if there is none.
  std::optional<LookupResult> Get(std::string_view key) const;

  // Approximate number of bytes of memory used by the stored entries,
  // including per-entry bookkeeping. Used to decide when to flush.
  size_t ApproximateMemoryUsage() const { return memory_usage_; }

  // Number of distinct keys stored.
  size_t size() const { return table_.size(); }
  bool empty() const { return table_.empty(); }

 private:
  struct Entry {
    uint64_t seq;
    std::string value;
  };

  // std::less<> (a "transparent" comparator) lets find() take a
  // std::string_view directly, instead of building a temporary std::string
  // (and allocating) on every lookup.
  std::map<std::string, Entry, std::less<>> table_;
  size_t memory_usage_ = 0;
};

}  // namespace tracedb

#endif  // TRACEDB_MEMTABLE_MEMTABLE_H_
