#include "db_impl.h"

#include <memory>
#include <utility>

#include "span_codec.h"

namespace tracedb {

// Declared in the public header (tracedb/db.h) but defined here, because
// creating a DBImpl requires its full definition, which the public header
// deliberately does not include.
Result<std::unique_ptr<TraceDB>> TraceDB::Open(const std::string& dir,
                                               const Options& options) {
  // Reject bad configuration up front, rather than failing confusingly later.
  if (dir.empty()) {
    return Status::InvalidArgument("dir must not be empty");
  }
  if (options.memtable_size_bytes == 0) {
    return Status::InvalidArgument("memtable_size_bytes must be > 0");
  }

  // Later steps: create and lock the directory, then recover from the
  // MANIFEST and write-ahead log here.

  // unique_ptr<DBImpl> converts to unique_ptr<TraceDB>, which converts to
  // Result<unique_ptr<TraceDB>>.
  return std::make_unique<DBImpl>();
}

Status DBImpl::Put(const Span& span) {
  if (closed_) return Status::InvalidArgument("database is closed");

  // Write path: 1. assign a sequence number, 2. append to the write-ahead log
  // (added in a later step), 3. insert into the memtable. The log must come
  // before the memtable, so a write is never visible without being durable.
  const uint64_t seq = next_seq_++;
  const std::string key = EncodeKey(span.key);
  const std::string value = EncodeValue(span);

  memtable_.Insert(seq, key, value);
  return Status::OK();
}

Result<std::optional<Span>> DBImpl::Get(const SpanKey& key) {
  if (closed_) return Status::InvalidArgument("database is closed");

  const std::string encoded_key = EncodeKey(key);
  std::optional<Memtable::LookupResult> entry = memtable_.Get(encoded_key);

  // Later step: if the memtable does not have the key, check the SSTables,
  // newest first.
  if (!entry) return std::nullopt;  // not found: OK, not an error

  // entry->value points into the memtable, so decode it now, before anything
  // can modify the memtable. DecodeSpan copies everything into the new Span.
  Result<Span> span = DecodeSpan(key, entry->value);
  if (!span.ok()) return span.status();
  return std::optional<Span>(std::move(span).value());
}

Status DBImpl::Close() {
  // Closing twice is allowed: the second call changes nothing.
  // Later steps: sync and close the write-ahead log, release the LOCK file.
  closed_ = true;
  return Status::OK();
}

}  // namespace tracedb
