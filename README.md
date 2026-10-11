# TraceDB

[![CI](https://github.com/NealJ02/TraceDB/actions/workflows/ci.yml/badge.svg)](https://github.com/NealJ02/TraceDB/actions/workflows/ci.yml)

An embedded storage engine for distributed-tracing data, written from scratch in C++20.

TraceDB is a log-structured merge (LSM) tree specialized for spans: data that is written roughly in time order, almost never updated, queried by time range and tags, and expired in bulk by age. It is a library, not a server, and depends on no other database or storage library.

> **Status:** early development. The project skeleton (build, tests, sanitizers, CI) and the on-disk span encoding are in place; the storage engine is not yet implemented.

The full specification is in [docs/DESIGN.md](docs/DESIGN.md).

## Building and testing

Requirements: CMake 3.20+ and a C++20 compiler (GCC 11+ or Clang 14+). GoogleTest is downloaded automatically on the first configure.

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To build and test with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-asan -DTRACEDB_SANITIZE=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## Layout

```
include/tracedb/   public headers
src/               implementation
tests/unit/        unit tests (GoogleTest)
docs/              design specification
```
