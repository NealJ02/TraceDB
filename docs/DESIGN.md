# TraceDB: An Embedded Storage Engine for Time-Series Trace Data

**Specification, v0.2**
**Author:** Neal Jain
**Status:** Draft. Sections marked *(implemented)* match the code.

---

## 1. Overview

TraceDB is an embedded key-value storage engine, written from scratch in C++, specialized for distributed-tracing data: spans that are written roughly in time order, almost never updated, and queried mostly by time range and tags.

It uses a log-structured merge (LSM) tree design: writes go to a write-ahead log and an in-memory sorted table, which is periodically flushed to immutable sorted files on disk and merged in the background by compaction.

**Motivation:** during my LinkedIn internship I stored trace metadata in MySQL and offloaded payloads to object storage. This project asks what storage built specifically for that workload would look like, and measures where a specialized engine wins and loses against a general-purpose database (SQLite).

TraceDB is a library, not a server. Callers link it into their program and call its API directly. It reads and writes its own files on disk and does not depend on any existing database or storage library.

### 1.1 Why an LSM tree rather than a B-tree

General-purpose databases (MySQL/InnoDB, SQLite) store tables as B-trees, which update fixed-size pages in place. An LSM tree never modifies data on disk; it writes new immutable files and merges them later. For trace data, that trade is favorable for four reasons, in order of importance:

1. **Retention is nearly free.** Trace data is deleted in bulk by age. Because keys start with the timestamp and files are immutable, old data sits in old files, and `DropBefore` deletes whole files. A B-tree must visit and rewrite every page holding old rows, and the space is not returned to the OS without a full `VACUUM`.
2. **Immutability makes crash safety much simpler.** Nothing already on disk is overwritten, so there are no torn pages and no multi-page split to make atomic. A new file becomes visible through a single atomic MANIFEST update.
3. **The workload is write-heavy, and its reads avoid the LSM tree's weak spot.** Every request writes spans; few are ever read. Most reads are recent time-range scans, not point lookups.
4. **Large, variable-size payloads fit naturally**, with no overflow-page chains.

**Caveats** (to be measured, not assumed):

- Time-ordered keys are also the best case for a B-tree: inserts append to the rightmost leaf. The write advantage is smaller than in the textbook random-key case.
- With `kEveryWrite`, both engines are dominated by fsync latency, so W1 may be close under per-write sync.
- A B-tree keyed or indexed by timestamp also touches only the requested window, so time-range scans (W2) are not an inherent LSM advantage. The difference depends on SQLite's schema ([Section 10](#10-benchmarks)).
- The LSM tree is worse at point lookups (several files may be checked) and has compaction work and latency spikes that a B-tree does not.

---

## 2. Goals and Non-Goals

### Goals

- **G1. Durability:** once `Put()` returns successfully (with sync enabled), the record survives a process crash.
- **G2. Correctness:** reads always reflect the latest successful write for a key, and never return corrupted data silently (corruption is detected via checksums and reported as an error).
- **G3. Fast appends:** sustain high write throughput for time-ordered inserts.
- **G4. Fast time-range scans:** a query over a narrow time window should touch only the data in that window, not the whole database.
- **G5. Bounded growth:** compaction and retention keep disk usage proportional to live data.
- **G6. Clarity:** code, tests, and documentation readable enough that a reviewer can understand the design in under 30 minutes.

### Non-goals (v1)

- SQL or any query language
- Networking, client/server mode, replication, or distribution
- Multi-key transactions
- Multiple concurrent writers (single writer, see [Section 9](#9-concurrency-model))
- Windows support (Linux and macOS only)
- Beating SQLite on every workload. The goal is to understand the tradeoffs.

---

## 3. Data Model

### 3.1 Span record *(implemented: `include/tracedb/span.h`)*

A **span** is one timed operation (one LLM call, one tool call). A **trace** is all spans sharing a `trace_id`; it is not stored as a separate object. `parent_id` links spans into a tree.

| Field         | C++ type                             | Notes                                                    |
|---------------|--------------------------------------|----------------------------------------------------------|
| `timestamp_ns`| `uint64_t` (ns since epoch)          | Span start time. Primary ordering field. Part of the key.|
| `trace_id`    | `std::array<uint8_t, 16>` (`TraceId`)| 128-bit ID as raw bytes. Part of the key.                |
| `span_id`     | `uint64_t`                           | Unique within a trace. Part of the key.                  |
| `parent_id`   | `uint64_t` (0 = root)                | Builds the span tree.                                    |
| `name`        | `std::string`                        | e.g. `llm_call`, `tool_call`                             |
| `duration_ns` | `uint64_t`                           |                                                          |
| `tags`        | `std::map<std::string, std::string>` | e.g. `agent`, `model`, `status`. The only filterable fields. |
| `payload`     | `std::string` (arbitrary bytes)      | The operation's content: prompts, responses, tool output. Often large (~200 B–20 KiB). Opaque to TraceDB. |

- The three key fields form `SpanKey`, whose defaulted `operator<=>` compares `timestamp_ns`, then `trace_id` byte by byte, then `span_id`. This **must** equal the byte order of the encoded key ([3.2](#32-key-encoding-implemented-srcspan_codech)).
- `tags` is a `std::map`, not an `unordered_map`, so a span always encodes to the same bytes.
- Every field has a default value, so a default-constructed `Span` holds no uninitialized data.

### 3.2 Key encoding *(implemented: `src/span_codec.h`)*

```
key = timestamp_ns (8 bytes, big-endian)
    | trace_id     (16 bytes, as-is)
    | span_id      (8 bytes, big-endian)
```

Total: 32 bytes, fixed width (`kKeySize`). Keys are compared as unsigned bytes (`memcmp` / `std::string` comparison). Big-endian puts the most significant byte first, so byte-wise comparison equals logical `SpanKey` order, and a time range maps to a contiguous range of keys. Little-endian would break this (e.g. 256 would sort before 1).

Because every key has the same size, keys are stored without a length prefix everywhere (WAL, SSTables, index blocks, footer).

### 3.3 Value encoding *(implemented: `src/span_codec.h`)*

The remaining fields are serialized as follows. "Length-prefixed" means a varint length followed by that many bytes.

```
version      u8          kSpanFormatVersion = 1
parent_id    fixed64     little-endian, 8 bytes
duration_ns  varint64    1-10 bytes
name         length-prefixed
tag_count    varint64
  tag key    length-prefixed   } repeated tag_count times,
  tag value  length-prefixed   } in strictly increasing key order
payload      length-prefixed
```

- **Varints** store 7 bits per byte, lowest bits first; the top bit of each byte is set when more bytes follow. A 64-bit value takes 1-10 bytes.
- **`parent_id` is fixed-width, not a varint:** span IDs are random 64-bit numbers, which take 9-10 bytes as varints but always 8 fixed.
- **The payload is last**, so all small fields sit at the front of the value.
- **Decoding is strict.** `DecodeSpan` returns `kCorruption` for an empty value, an unknown version, any truncated or malformed field, a varint longer than 10 bytes or overflowing 64 bits, a length longer than the remaining bytes, tags out of order or duplicated, and any bytes left over after the payload.
- **Decoding never trusts sizes from disk:** lengths are checked against the remaining input before use, and no memory is reserved based on a count read from disk.
- **The decoded `Span` owns its data**, so the input buffer may be freed or reused afterwards.
- Changing this layout changes the on-disk format. Bump `kSpanFormatVersion` instead of changing version 1.

Worked example: a span with `parent_id = 0`, `duration_ns = 1,250,000,000`, `name = "llm_call"`, `tags = {model: m1, status: ok}`, `payload = "hi"` encodes to these 46 bytes (pinned by a golden-bytes test):

```
01 00 00 00 00 00 00 00 00 80 f9 85 d4 04 08 6c 6c 6d 5f 63 61 6c 6c 02
05 6d 6f 64 65 6c 02 6d 31 06 73 74 61 74 75 73 02 6f 6b 02 68 69
```

### 3.4 Write semantics

- `Put()` with an existing key overwrites it (last write wins).
- Every write is assigned a monotonically increasing 64-bit sequence number. When multiple versions of a key exist, the highest sequence number wins.
- Per-key deletes are **not** supported in v1. Data is removed only by retention (`DropBefore`, [Section 4](#4-public-api-c20)), which matches how trace data is actually managed. Tombstones are listed as a stretch goal.
- **A missing key is not an error.** `Get` returns an OK result holding `std::nullopt`.

---

## 4. Public API (C++20)

```cpp
struct Options {
  size_t   memtable_size_bytes   = 4 << 20;  // flush threshold (4 MiB)
  size_t   block_size_bytes      = 4 << 10;  // SSTable data block (4 KiB)
  SyncMode sync_mode             = SyncMode::kEveryWrite;
                                   // kEveryWrite | kGroupCommit | kNone
  int      l0_compaction_trigger = 4;        // L0 files before compaction
};

struct TimeRange { uint64_t start_ns; uint64_t end_ns; };  // [start, end)

struct TagFilter { std::string key; std::string value; };   // equality only

class TraceDB {
 public:
  static Result<std::unique_ptr<TraceDB>> Open(const std::string& dir,
                                               const Options& opts);

  Status Put(const Span& span);
  Result<std::optional<Span>> Get(const SpanKey& key);

  // Returns spans with timestamp in range, in key order, optionally
  // filtered to spans whose tags match all filters.
  Result<std::unique_ptr<SpanIterator>> Scan(
      const TimeRange& range,
      const std::vector<TagFilter>& filters = {});

  // Removes all spans with timestamp < cutoff_ns. Implemented by deleting
  // whole SSTables where possible and filtering the rest at compaction.
  Status DropBefore(uint64_t cutoff_ns);

  Status Flush();       // force memtable -> SSTable
  Status CompactAll();  // force full compaction (testing/benchmarks)
  Status Close();
};
```

### 4.1 Errors *(implemented: `include/tracedb/status.h`)*

Errors are returned, never thrown.

- **`Status`** is OK or an error code plus a message. **`Result<T>`** holds either a `T` or a non-OK `Status`; both convert implicitly, so a function can `return value;` or `return Status::IOError(...);`.
- Both are `[[nodiscard]]`: ignoring a returned `Status` is a compiler warning, so a failed write or fsync cannot be dropped silently.

| Code | Meaning |
|---|---|
| `kIOError` | The OS reported a failure (open, write, fsync, ...) |
| `kCorruption` | Checksum mismatch, bad magic number, malformed encoding |
| `kInvalidArgument` | The caller passed bad input |
| `kNotFound` | Something that should exist does not (e.g. a file listed in the MANIFEST). **Not** used for a missing key. |

---

## 5. Architecture

```
              Put()                          Get() / Scan()
                |                                  |
                v                                  v
        +---------------+                +------------------+
        | Write-ahead   |                | Merging iterator |
        | log (WAL)     |                | (newest wins)    |
        +---------------+                +------------------+
                |                         |        |       |
                v                         v        v       v
        +---------------+         memtable   SSTable  SSTable ...
        | Memtable      |
        | (sorted, RAM) |
        +---------------+
                | full
                v
        +---------------+     compaction    +----------------+
        | SSTables (L0) | ----------------> | SSTables (L1)  |
        | immutable     |                   | non-overlapping|
        +---------------+                   +----------------+

        MANIFEST tracks which SSTables are live at any moment.
```

### 5.1 Write path

1. Assign a sequence number.
2. Append the record to the WAL; fsync according to `sync_mode`.
3. Insert into the memtable.
4. If the memtable exceeds `memtable_size_bytes`, flush ([5.4](#54-flush)).

### 5.2 Read path (`Get`)

1. Check the memtable.
2. Check L0 SSTables newest to oldest, then L1. L1 files do not overlap, so at most one L1 file can contain the key. Skip any SSTable whose key range (from manifest metadata) cannot contain the key.
3. Return the first match found (it has the highest sequence number).

### 5.3 Read path (`Scan`)

1. Select SSTables whose `[min_ts, max_ts]` overlaps the requested range. Others are never opened. Because keys begin with the timestamp, this is the same mechanism as key-range pruning in `Get`.
2. Build a merging iterator over the memtable plus selected SSTables, seeking each to the range start.
3. For duplicate keys, emit only the highest sequence number.
4. Apply tag filters to each emitted span (v1: decode and check).
5. Stop at the range end.

### 5.4 Flush

1. Freeze the current memtable; start a new memtable and a new WAL file.
2. Write the frozen memtable to a new SSTable file ([6.2](#62-sstable-file)).
3. fsync the SSTable file and its directory.
4. Record the new SSTable in the MANIFEST (atomic, [6.3](#63-manifest-and-current)).
5. Delete the old WAL file.

A crash at any step must be safe: before step 4 completes, the old WAL still exists and recovery replays it; the orphan SSTable is garbage-collected.

### 5.5 Compaction (v1: two-level leveled)

- L0 holds freshly flushed files, whose key ranges may overlap. L1 holds compacted files, whose key ranges do not overlap.
- When L0 has at least `l0_compaction_trigger` files, merge all L0 files plus any overlapping L1 files into new, non-overlapping L1 files (target size ~16 MiB each).
- During the merge, drop older versions of duplicate keys and spans older than the retention cutoff.
- New files are installed via a single MANIFEST edit; old files are deleted only after the edit is durable.

**Why two levels are enough.** General-purpose LSM engines use many levels (LevelDB uses up to 7) because random keys make new data overlap most of a large level, so each compaction would rewrite a large part of it. Here keys start with the timestamp and arrive roughly in order: new L0 files cover recent time and older L1 files cover the past, so a compaction touches at most the newest L1 file or two, regardless of total data size. Retention also bounds L1's size. The cost case is **late-arriving data**, which overlaps an old L1 file and forces that whole file to be rewritten (see Q3).

**Stretch:** time-window compaction (only merge files whose time ranges fall in the same window), which suits time-series data better and makes `DropBefore` nearly free.

### 5.6 Recovery (`Open`)

1. Read `CURRENT` to find the active MANIFEST; replay it to get the live SSTables.
2. Delete any SSTable files not referenced by the manifest (orphans).
3. Replay WAL file(s) into a fresh memtable. Stop at the first record with an invalid checksum or truncated length: a torn final write is expected after a crash and is discarded. A bad record that is **not** at the tail is reported as `kCorruption`.
4. Restore the next sequence number as `max(seen) + 1`.

---

## 6. On-Disk Formats

**Byte order rule:** fields that are compared as bytes (the key's `timestamp_ns` and `span_id`) are big-endian. Every other integer is little-endian: it is only ever read back as a number, and little-endian matches x86 and ARM. All checksums are CRC32C.

Integers are written with explicit shifts (`src/util/coding.h`), never by copying a struct or integer's memory, so the format does not depend on the machine's byte order, padding, or compiler.

### 6.1 WAL record

```
+------------+------------+-----------+---------+---------+---------+
| crc32c u32 | length u32 | seq u64   | type u8 | key 32B | value   |
+------------+------------+-----------+---------+---------+---------+
```

- `length` is the number of bytes **after** the length field: `8 + 1 + 32 + len(value)`.
- The CRC covers everything after the CRC field, including `length`, so a corrupted length is detected.
- A `length` larger than the bytes remaining in the file means a truncated record: a torn tail if it is the last record, `kCorruption` otherwise.
- `type`: `1` = Put (reserved: `2` = Delete for future tombstones).
- `value` is the encoded span value ([3.3](#33-value-encoding-implemented-srcspan_codech)).

### 6.2 SSTable file

```
[data block 0][data block 1] ... [data block N]
[index block]
[footer]
```

- **Data block:** sorted entries `(key 32B, seq fixed64, value_len varint, value)`, followed by a fixed32 CRC32C of the block. `block_size_bytes` (4 KiB) is a **target**: a block is finished once it reaches the target, so an entry larger than 4 KiB (e.g. a 20 KiB payload) gets a larger block of its own.
- **Index block:** one entry per data block: `(last_key_in_block 32B, offset fixed64, size fixed64)`.
- **Footer (fixed size, 116 bytes):**

| Field | Size |
|---|---|
| index offset | 8 |
| index size | 8 |
| entry count | 8 |
| `min_ts` | 8 |
| `max_ts` | 8 |
| `min_key` | 32 |
| `max_key` | 32 |
| format version | 4 |
| magic number | 8 |

`min_ts`/`max_ts` are the first 8 bytes of `min_key`/`max_key` decoded, so they are redundant; they are kept for readability.

Lookups read the footer (at `file_size - 116`), check the magic number, binary-search the index block, then read a single data block.

### 6.3 MANIFEST and CURRENT

- **`MANIFEST-<n>`:** append-only log of edits, each framed like a WAL record (`crc32c`, `length`, payload) so corruption and torn tails are detected the same way:
  - `AddFile(file_number, level, min_key, max_key, min_ts, max_ts, size)`
  - `RemoveFile(file_number)`
  - `SetNextFileNumber`, `SetLastSequence`
- **`CURRENT`:** contains the name of the active MANIFEST. Updated by writing a temp file, fsync, then an atomic `rename()`, which makes switching manifests atomic.

### 6.4 Directory layout

```
<db_dir>/
  CURRENT
  MANIFEST-000001
  000004.wal
  000005.sst
  000006.sst
  LOCK          # prevents two processes opening the same DB
```

All file numbers come from one counter (`SetNextFileNumber`), so names never collide.

---

## 7. Error Handling and Corruption

- Every block, WAL record, and manifest edit is checksummed. A checksum mismatch returns `kCorruption`; data is never silently returned.
- Every decoder treats its input as untrusted: it never reads past the end of its input, whatever the bytes contain, and reports malformed data as `kCorruption` rather than crashing.
- I/O errors during a write leave the DB in a state where the failed write is either fully absent or fully present after recovery, never partial.
- After an fsync failure, the DB enters a read-only error state (retrying fsync after a failure is unsafe on Linux).

---

## 8. Testing Strategy

Correctness is the central goal of this project. Testing gets as much effort as the engine itself.

### 8.1 Unit tests (GoogleTest)

- Key/value encoding round-trips, including edge cases (empty fields, max values, 20 KiB payloads, binary bytes). *(implemented)*
- **Golden-bytes tests** that pin the exact on-disk encoding, so an accidental format change fails even when encoder and decoder change together. *(implemented)*
- **Ordering property test:** for 10,000 random key pairs, comparing encoded bytes agrees with comparing `SpanKey`s. *(implemented)*
- **Truncation sweeps:** every prefix of a valid encoding must decode to `kCorruption`, never over-read. *(implemented)*
- WAL write/read, including truncated and corrupted tails.
- SSTable build/read, block boundaries, index search.
- Merging iterator with duplicates across sources.

### 8.2 Differential (model-based) testing

- Run thousands of random operations (`Put`, `Get`, `Scan`, `Flush`, `CompactAll`, `DropBefore`, reopen) against both TraceDB and a reference model (`std::map<SpanKey, Span>`).
- After every operation, check that the results agree exactly.
- Draw keys from a small key space so that overwrites and successful lookups are frequent.
- Use a seeded RNG so any failure is reproducible from its seed.
- The harness is built first, with only `Put`/`Get`, and each later feature adds its operations to it.

### 8.3 Crash testing

- Fork a child process that performs random writes and reports each acknowledged write to the parent through a pipe.
- The parent sends `SIGKILL` at a random moment, reopens the DB, and verifies that every acknowledged write (with sync on) is present and no data is corrupted.
- Repeat for many iterations with different seeds.

### 8.4 Fault injection

- An `Env`/filesystem abstraction layer lets tests inject failures: short writes, fsync errors, and bit flips in data blocks. All file I/O goes through `Env` from the start.
- Verify that corruption is detected and reported, never silently returned.

### 8.5 Tooling *(implemented)*

- CMake build; GoogleTest is fetched automatically (`FetchContent`, pinned to v1.15.2).
- `-DTRACEDB_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer, with `-fno-sanitize-recover=all` so undefined behavior fails the test. ThreadSanitizer is added once background threads exist (it needs a separate build).
- Warnings: `-Wall -Wextra -Wpedantic -Wshadow` on project code.
- CI via GitHub Actions on every push and pull request: {Ubuntu, macOS} × {sanitizers off, on}.

---

## 9. Concurrency Model

**v1:** Single-threaded. All operations, including flush and compaction, run synchronously on the caller's thread. Simple and easy to verify. Consequence: a `Put` that triggers a flush or compaction is slow, which will show in p99 write latency.

**v2 (stretch):**

- One background thread for flush and compaction.
- Readers get a consistent snapshot (memtable + immutable memtable + SSTable set) via reference-counted `Version` objects, so compaction can swap files without blocking reads.
- Writes remain single-writer, serialized by a mutex.

---

## 10. Benchmarks

Compare TraceDB against SQLite (WAL journal mode, index on timestamp, same sync settings for a fair comparison) on synthetic data modeled on real agent traces (realistic tag cardinality, payload sizes from ~200 B to ~20 KiB). Benchmarks use a `Release` build, never a sanitizer build.

| Workload | Description                                                    | Expectation |
|----------|----------------------------------------------------------------|-------------|
| W1       | Ingest: time-ordered inserts with slight out-of-order jitter   | TraceDB ahead, but close under `kEveryWrite` (fsync-bound) |
| W2       | Recent range scan: last 5 minutes out of 24 hours of data      | Possibly close; depends on SQLite schema |
| W3       | Filtered scan: `status=error` within a 1-hour window           | Uncertain: no tag index in v1 |
| W4       | Point lookups by key                                           | SQLite may win |
| W5       | Mixed: 90% writes, 10% recent scans                            | Models the real read/write ratio |

**SQLite schema matters.** With a rowid table plus a secondary index on timestamp, each matching row costs an extra lookup into the table. A `WITHOUT ROWID` table keyed on `(timestamp, trace_id, span_id)` avoids that. Report which schema is used, ideally both.

**Metrics:** throughput (ops/sec), p50/p99 latency, write amplification (bytes written to disk ÷ bytes written by the user), and space amplification (disk size ÷ live data size).

Every result goes in the README with an explanation of *why* TraceDB wins or loses on that workload.

---

## 11. Milestones

| Milestone | Timing             | Deliverable | Status |
|-----------|--------------------|-------------|--------|
| **M0**    | Winter break 2026  | Finalize this spec. Set up repo, CMake, GoogleTest, sanitizers, CI. Implement encoding and `Status`/`Result` types. | Done |
| **M1**    | Weeks 1–2          | WAL + memtable + recovery. `Put`/`Get` work in memory with a crash-safe WAL. Differential test harness running from day one. | Next |
| **M2**    | Weeks 3–4          | SSTables + flush + manifest. Data persists across restarts. `Get` reads across sources. | |
| **M3**    | Weeks 5–6          | `Scan` + merging iterator + tag filters. Time-range pruning via manifest min/max timestamps. | |
| **M4**    | Weeks 7–8          | Compaction + `DropBefore`. Crash tests and fault-injection tests passing. | |
| **M5**    | Weeks 9–10         | Benchmarks + README. SQLite comparison, results write-up, design doc with tradeoffs. | |

**Stretch goals** (after M5, in rough priority order): Bloom filters per SSTable (see Q6), payload separation into a blob log (mirrors the LinkedIn two-tier design), block compression, time-window compaction, a per-SSTable tag index, a background compaction thread, and tombstones for per-key deletes.

---

## 12. Repository Layout

Current files, plus the planned layout for later milestones.

```
tracedb/
  include/tracedb/      # public headers
    status.h            #   Status, Result<T>          (implemented)
    span.h              #   Span, SpanKey, TraceId     (implemented)
    db.h, options.h     #   planned
  src/
    status.cc           # (implemented)
    span_codec.{h,cc}   # key/value encoding           (implemented)
    util/coding.{h,cc}  # fixed/varint/length-prefixed (implemented)
    wal/                # log writer/reader
    memtable/
    sstable/            # builder, reader, block, index
    manifest/
    compaction/
    iterator/           # merging iterator
    env/                # filesystem abstraction (real + fault-injecting)
  tests/
    unit/               # (implemented)
    differential/
    crash/
  bench/                # workload generator, SQLite baseline, runner
  docs/
    DESIGN.md
    BENCHMARKS.md
  .github/workflows/ci.yml
  README.md
  CMakeLists.txt
```

---

## 13. Open Questions

- **Q1.** Should payloads live inline or in a separate blob log from the start? (Inline is simpler; separation reduces compaction write amplification for large payloads.)
- **Q2.** Memtable structure: `std::map` (simple) or a hand-written skip list (lock-free concurrent reads, needed for v2)? Leaning: `std::map` behind a small interface for v1. It must be a *balanced* tree: time-ordered inserts would degrade an unbalanced BST into a linked list.
- **Q3.** How much out-of-order arrival should the benchmark data have, and how does it affect L0 overlap and the cost of rewriting L1 files for late data?
- **Q4.** Default sync mode for benchmarks: `kEveryWrite` is fair to SQLite's FULL sync, but group commit is more realistic for tracing workloads. Report both.
- **Q5.** Is a tag index worth it in v1, or does time-range pruning plus in-range filtering make it unnecessary for realistic window sizes?
- **Q6.** Trace lookups. `Get` requires the full key, including the timestamp, but the common real query is "all spans of trace X". Should there be a `GetTrace(trace_id)`, backed by a per-SSTable Bloom filter on `trace_id`? A Bloom filter on full keys helps `Get` little here, since key-range pruning already narrows a lookup to a few L0 files and one L1 file; one on `trace_id` would let a trace lookup skip ~99% of files. (Bloom filters are per SSTable: they answer "definitely not in this file", not "which level".)

---

## 14. Success Criteria

- All tests pass under ASan/UBSan, including 10,000+ randomized crash iterations with zero lost acknowledged writes.
- Benchmarks are reproducible from a single script.
- The README explains the design, tradeoffs, and benchmark results clearly enough that a reviewer understands the project in under 30 minutes.
- I can explain and defend every major design decision in an interview.

---

## Changelog

**v0.2**
- 1.1: added the rationale for an LSM tree over a B-tree, with caveats.
- 3.1: C++ types for every field; trace vs span; `std::map` tags for deterministic encoding.
- 3.2: keys compared as unsigned bytes; fixed size means no length prefix.
- 3.3: specified the exact value layout; `parent_id` fixed-width; strict decoding rules; worked example.
- 3.4: a missing key is `nullopt`, not an error.
- 4.1: `Status`/`Result` and the meaning of each error code.
- 5.5: renamed "simple tiered" to two-level leveled compaction; explained why no L2.
- 6: byte-order rule; WAL `length` field defined; block size is a target; footer sizes; MANIFEST framing.
- 8: golden-bytes, ordering-property and truncation tests; tooling as built.
- 10: expected outcomes; SQLite schema caveat.
- 13: Q2 leaning; Q6 trace lookups and Bloom filters.

**v0.1:** initial specification.
