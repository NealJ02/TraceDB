#ifndef TRACEDB_STATUS_H_
#define TRACEDB_STATUS_H_

#include <cassert>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace tracedb {

// The outcome of an operation that returns no value: either OK, or an error
// code plus a human-readable message. Errors are always returned, never thrown.
//
// [[nodiscard]] makes the compiler warn when a caller ignores a Status, so a
// failed write or fsync can never be dropped silently. To ignore one on
// purpose, cast it to void: (void)file->Close();
class [[nodiscard]] Status {
 public:
  enum class Code {
    kOk,
    kNotFound,         // something that should exist does not (e.g. a file).
                       // A missing key is NOT an error: Get returns nullopt.
    kCorruption,       // checksum mismatch, bad magic, malformed encoding
    kInvalidArgument,  // caller passed bad input
    kIOError,          // the OS reported a failure (open, write, fsync, ...)
  };

  Status() = default;  // OK

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg) {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(Code::kIOError, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  bool IsIOError() const { return code_ == Code::kIOError; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  // "OK", or "<code name>: <message>", e.g. "Corruption: bad crc in block 3".
  std::string ToString() const;

 private:
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

// Either a value of type T or a non-OK Status, never both.
//
// Both alternatives convert implicitly, so a function returning Result<T> can
// `return value;` on success and `return Status::IOError(...);` on failure.
template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, Status>,
                "Use Status directly instead of Result<Status>");

 public:
  // Success. Accepts anything T can be constructed from, not just T itself,
  // so `return std::nullopt;` works for Result<std::optional<X>> and
  // `return std::make_unique<Derived>();` works for
  // Result<std::unique_ptr<Base>>. It is implicit only when U converts to T
  // implicitly, mirroring std::expected.
  template <typename U = T>
    requires(std::is_constructible_v<T, U &&> &&
             !std::is_same_v<std::remove_cvref_t<U>, Status> &&
             !std::is_same_v<std::remove_cvref_t<U>, Result>)
  explicit(!std::is_convertible_v<U &&, T>) Result(U&& value)
      : data_(std::in_place_index<1>, std::forward<U>(value)) {}

  // Failure. Passing an OK Status is a bug: the Result would hold neither a
  // value nor an error.
  Result(Status status) : data_(std::in_place_index<0>, std::move(status)) {
    assert(!std::get<0>(data_).ok() && "Result constructed from OK Status");
  }

  bool ok() const { return data_.index() == 1; }

  // The error, or an OK Status if this Result holds a value.
  const Status& status() const {
    static const Status kOk;
    return ok() ? kOk : std::get<0>(data_);
  }

  // Accessing the value of a failed Result is a bug.
  T& value() & {
    assert(ok() && "value() called on a failed Result");
    return std::get<1>(data_);
  }
  const T& value() const& {
    assert(ok() && "value() called on a failed Result");
    return std::get<1>(data_);
  }
  // Lets move-only values be taken out: std::move(result).value().
  T&& value() && {
    assert(ok() && "value() called on a failed Result");
    return std::get<1>(std::move(data_));
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  std::variant<Status, T> data_;
};

}  // namespace tracedb

// Evaluates `expr` (which must produce a Status) and returns it from the
// enclosing function if it is not OK.
#define TRACEDB_RETURN_IF_ERROR(expr)                    \
  do {                                                   \
    ::tracedb::Status tracedb_status_macro_ = (expr);    \
    if (!tracedb_status_macro_.ok()) {                   \
      return tracedb_status_macro_;                      \
    }                                                    \
  } while (0)

#endif  // TRACEDB_STATUS_H_
