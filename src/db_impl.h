#ifndef TRACEDB_DB_IMPL_H_
#define TRACEDB_DB_IMPL_H_

#include <cstdint>
#include <optional>

#include "memtable/memtable.h"
#include "tracedb/db.h"
#include "tracedb/span.h"
#include "tracedb/status.h"

namespace tracedb {

// The implementation behind the public TraceDB interface. Users only ever see
// it as a TraceDB (via TraceDB::Open), so everything here can change without
// affecting the public header.
//
// Current state: all data lives in a single in-memory memtable. The
// write-ahead log, SSTables and flushing are added in later steps.
class DBImpl final : public TraceDB {
 public:
  // Use TraceDB::Open, which validates the directory and options first.
  // (They are not stored yet: the directory is needed once data is stored on
  // disk, and the options once the memtable is flushed. Fields are added in
  // the step that first uses them.)
  DBImpl() = default;

  Status Put(const Span& span) override;
  Result<std::optional<Span>> Get(const SpanKey& key) override;
  Status Close() override;

 private:
  Memtable memtable_;

  // Sequence number for the next write. Starts at 1, so 0 never belongs to a
  // real write.
  uint64_t next_seq_ = 1;

  bool closed_ = false;
};

}  // namespace tracedb

#endif  // TRACEDB_DB_IMPL_H_
