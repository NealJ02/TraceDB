#include "tracedb/status.h"

namespace tracedb {

std::string Status::ToString() const {
  const char* prefix = "";
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      prefix = "Not found: ";
      break;
    case Code::kCorruption:
      prefix = "Corruption: ";
      break;
    case Code::kInvalidArgument:
      prefix = "Invalid argument: ";
      break;
    case Code::kIOError:
      prefix = "IO error: ";
      break;
  }
  return prefix + msg_;
}

}  // namespace tracedb
