#ifndef TRACEDB_DB_H_
#define TRACEDB_DB_H_

#include <memory>
#include <optional>
#include <string>

#include "tracedb/options.h"
#include "tracedb/span.h"
#include "tracedb/status.h"

namespace tracedb {

// An embedded database of spans, ordered by SpanKey.
//
// This class is only the public interface: every method is pure virtual, and
// the implementation lives in a private class (DBImpl) that users never see.
// Create a database with TraceDB::Open, which returns an owning pointer:
//
//   Result<std::unique_ptr<TraceDB>> opened = TraceDB::Open(dir, Options{});
//   if (!opened.ok()) { /* handle opened.status() */ }
//   std::unique_ptr<TraceDB> db = std::move(opened).value();
//
//   Status s = db->Put(span);
//   Result<std::optional<Span>> got = db->Get(span.key);
//
// The database is destroyed (and its resources released) when the
// unique_ptr goes out of scope.
//
// Not thread-safe: all calls must come from one thread at a time.
class TraceDB {
 public:
  // Opens the database stored in directory `dir`.
  //
  // Fails with InvalidArgument if `dir` is empty or `options` is invalid.
  // (For now the database lives only in memory and nothing is read from or
  // written to `dir`; disk storage is added with the write-ahead log.)
  static Result<std::unique_ptr<TraceDB>> Open(const std::string& dir,
                                               const Options& options);

  // Declaring the (deleted) copy constructor below stops the compiler from
  // generating a default constructor, which the implementation class needs to
  // construct this base part. So declare it explicitly.
  TraceDB() = default;

  // A database handle cannot be copied: two copies would both think they own
  // the same data.
  TraceDB(const TraceDB&) = delete;
  TraceDB& operator=(const TraceDB&) = delete;

  // Virtual, so that deleting through a TraceDB* (as std::unique_ptr<TraceDB>
  // does) runs the implementation's destructor too.
  virtual ~TraceDB() = default;

  // Stores `span`, replacing any existing span with the same key.
  //
  // Fails with InvalidArgument if the database is closed.
  virtual Status Put(const Span& span) = 0;

  // Returns the span stored under `key`. A missing key is not an error: the
  // result is OK and holds std::nullopt.
  //
  // Fails with InvalidArgument if the database is closed, or Corruption if the
  // stored data cannot be decoded.
  virtual Result<std::optional<Span>> Get(const SpanKey& key) = 0;

  // Closes the database. Afterwards, Put and Get fail with InvalidArgument.
  // Calling Close more than once is allowed and does nothing.
  virtual Status Close() = 0;
};

}  // namespace tracedb

#endif  // TRACEDB_DB_H_
