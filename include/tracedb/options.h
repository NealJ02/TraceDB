#ifndef TRACEDB_OPTIONS_H_
#define TRACEDB_OPTIONS_H_

#include <cstddef>

namespace tracedb {

// Settings passed to TraceDB::Open. Every field has a sensible default, so
// Options{} is a valid configuration.
//
// Fields are added in the step that first uses them (block size, sync mode,
// compaction trigger, ...), so every option listed here has an effect.
struct Options {
  // Once the memtable's approximate memory usage reaches this many bytes, it
  // is flushed to an SSTable on disk. Must be greater than 0.
  // (Tracked now; enforced once flushing exists.)
  size_t memtable_size_bytes = 4 << 20;  // 4 MiB
};

}  // namespace tracedb

#endif  // TRACEDB_OPTIONS_H_
