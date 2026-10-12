#include "memtable/memtable.h"

#include <cassert>

namespace tracedb {

namespace {

// Estimated memory used by one entry beyond its key and value bytes:
//   - the tree node's parent/left/right pointers and color (padded to a
//     pointer's width),
//   - the std::string object holding the key,
//   - the Entry: its seq and the std::string object holding the value,
//   - a rough allowance for the allocator's own bookkeeping.
// The exact figure depends on the standard library and allocator; an estimate
// is enough to decide when to flush.
constexpr size_t kPerEntryOverhead = 4 * sizeof(void*) + sizeof(std::string) +
                                     sizeof(uint64_t) + sizeof(std::string) +
                                     16;

}  // namespace

void Memtable::Insert(uint64_t seq, std::string_view key,
                      std::string_view value) {
  // One search of the tree decides between the two cases.
  auto it = table_.find(key);

  if (it != table_.end()) {
    // Existing key: reuse the node and the stored key; replace the value.
    Entry& entry = it->second;
    assert(seq > entry.seq && "sequence numbers must increase");
    memory_usage_ -= entry.value.size();
    entry.seq = seq;
    entry.value.assign(value);
    memory_usage_ += entry.value.size();
    return;
  }

  // New key: the map allocates a node holding copies of the key and value.
  table_.emplace(std::string(key), Entry{seq, std::string(value)});
  memory_usage_ += key.size() + value.size() + kPerEntryOverhead;
}

std::optional<Memtable::LookupResult> Memtable::Get(
    std::string_view key) const {
  auto it = table_.find(key);
  if (it == table_.end()) return std::nullopt;
  // A view of the stored value's bytes, not a copy.
  return LookupResult{it->second.seq, it->second.value};
}

}  // namespace tracedb
