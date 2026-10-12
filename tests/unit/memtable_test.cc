#include "memtable/memtable.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace tracedb {
namespace {

// Looks up `key` and returns its value as a std::string, or std::nullopt.
// Copies the value so tests can compare it after further modifications.
std::optional<std::string> GetValue(const Memtable& mem, std::string_view key) {
  std::optional<Memtable::LookupResult> r = mem.Get(key);
  if (!r) return std::nullopt;
  return std::string(r->value);
}

TEST(MemtableTest, EmptyByDefault) {
  Memtable mem;
  EXPECT_TRUE(mem.empty());
  EXPECT_EQ(mem.size(), 0u);
  EXPECT_EQ(mem.ApproximateMemoryUsage(), 0u);
  EXPECT_FALSE(mem.Get("missing").has_value());
}

TEST(MemtableTest, InsertThenGet) {
  Memtable mem;
  mem.Insert(/*seq=*/1, "key", "value");

  std::optional<Memtable::LookupResult> r = mem.Get("key");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->seq, 1u);
  EXPECT_EQ(r->value, "value");
  EXPECT_EQ(mem.size(), 1u);
  EXPECT_FALSE(mem.empty());
}

TEST(MemtableTest, GetMissingKeyAmongOthers) {
  Memtable mem;
  mem.Insert(1, "a", "1");
  mem.Insert(2, "c", "3");
  EXPECT_FALSE(mem.Get("b").has_value());   // between two keys
  EXPECT_FALSE(mem.Get("").has_value());    // before all keys
  EXPECT_FALSE(mem.Get("d").has_value());   // after all keys
  EXPECT_FALSE(mem.Get("aa").has_value());  // extends an existing key
}

TEST(MemtableTest, OverwriteReplacesValueAndSeq) {
  Memtable mem;
  mem.Insert(1, "key", "old");
  mem.Insert(2, "key", "new");

  std::optional<Memtable::LookupResult> r = mem.Get("key");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->seq, 2u);
  EXPECT_EQ(r->value, "new");
  EXPECT_EQ(mem.size(), 1u);  // still one key
}

TEST(MemtableTest, ManyKeysEachRetrievable) {
  Memtable mem;
  for (int i = 0; i < 1000; ++i) {
    mem.Insert(static_cast<uint64_t>(i + 1), "key" + std::to_string(i),
               "value" + std::to_string(i));
  }
  EXPECT_EQ(mem.size(), 1000u);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(GetValue(mem, "key" + std::to_string(i)),
              "value" + std::to_string(i));
  }
}

TEST(MemtableTest, EmptyValueIsStored) {
  Memtable mem;
  mem.Insert(1, "key", "");
  std::optional<Memtable::LookupResult> r = mem.Get("key");
  ASSERT_TRUE(r.has_value());  // present, with an empty value
  EXPECT_TRUE(r->value.empty());
}

// Keys and values are arbitrary bytes, including 0x00, which must not end a
// key early.
TEST(MemtableTest, BinaryKeysAndValues) {
  Memtable mem;
  const std::string k1("a\0b", 3);
  const std::string k2("a\0c", 3);  // differs from k1 only after the 0x00
  const std::string k3("\xff\x00", 2);
  const std::string v1("\x00\x01\x02", 3);
  const std::string v2("\xff\xff", 2);

  mem.Insert(1, k1, v1);
  mem.Insert(2, k2, v2);
  mem.Insert(3, k3, v1);

  EXPECT_EQ(mem.size(), 3u);
  EXPECT_EQ(GetValue(mem, k1), v1);
  EXPECT_EQ(GetValue(mem, k2), v2);
  EXPECT_EQ(GetValue(mem, k3), v1);
  EXPECT_FALSE(mem.Get(std::string_view("a", 1)).has_value());
}

// Get takes a string_view, so the key can live inside some other buffer
// (e.g. a block read from disk) without being copied into a std::string.
TEST(MemtableTest, LookupWithViewIntoAnotherBuffer) {
  Memtable mem;
  mem.Insert(1, "key", "value");
  const std::string buffer = "xxkeyxx";
  std::string_view key = std::string_view(buffer).substr(2, 3);  // "key"
  EXPECT_EQ(GetValue(mem, key), "value");
}

// The memtable must keep its own copies: destroying the caller's buffers must
// not affect stored entries. If it kept views instead, ASan would report a
// use-after-free here.
TEST(MemtableTest, OwnsItsData) {
  Memtable mem;
  {
    std::string key(100, 'k');    // long enough to live on the heap
    std::string value(100, 'v');
    mem.Insert(1, key, value);
    key.assign(100, 'X');         // overwrite the caller's buffers...
    value.assign(100, 'X');
  }                               // ...then free them
  EXPECT_EQ(GetValue(mem, std::string(100, 'k')), std::string(100, 'v'));
}

// The returned view points into the memtable's storage, not a copy.
TEST(MemtableTest, GetReturnsViewIntoStorage) {
  Memtable mem;
  mem.Insert(1, "key", "value");
  std::string_view first = mem.Get("key")->value;
  std::string_view second = mem.Get("key")->value;
  EXPECT_EQ(first.data(), second.data());  // same bytes both times
}

TEST(MemtableTest, MemoryUsageGrowsWithNewKeys) {
  Memtable mem;
  mem.Insert(1, "key1", "value1");
  size_t after_one = mem.ApproximateMemoryUsage();
  // At least the key and value bytes, plus some bookkeeping.
  EXPECT_GT(after_one, std::string("key1").size() + std::string("value1").size());

  mem.Insert(2, "key2", std::string(1000, 'v'));
  size_t after_two = mem.ApproximateMemoryUsage();
  EXPECT_GE(after_two - after_one, 4u + 1000u);
}

// Overwriting changes usage by exactly the change in value size: the key and
// the per-entry bookkeeping were already counted.
TEST(MemtableTest, MemoryUsageOnOverwrite) {
  Memtable mem;
  mem.Insert(1, "key", std::string(100, 'a'));
  size_t base = mem.ApproximateMemoryUsage();

  mem.Insert(2, "key", std::string(250, 'b'));  // grows by 150
  EXPECT_EQ(mem.ApproximateMemoryUsage(), base + 150);

  mem.Insert(3, "key", std::string(40, 'c'));   // shrinks by 210
  EXPECT_EQ(mem.ApproximateMemoryUsage(), base - 60);

  mem.Insert(4, "key", std::string(100, 'd'));  // back to the start
  EXPECT_EQ(mem.ApproximateMemoryUsage(), base);
}

// Inserting and then overwriting every key many times must not make the
// estimate drift: it depends only on what is currently stored.
TEST(MemtableTest, MemoryUsageDoesNotDrift) {
  Memtable mem;
  uint64_t seq = 0;
  for (int i = 0; i < 100; ++i) {
    mem.Insert(++seq, "key" + std::to_string(i), "v");
  }
  size_t base = mem.ApproximateMemoryUsage();

  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 100; ++i) {
      mem.Insert(++seq, "key" + std::to_string(i),
                 std::string(static_cast<size_t>(round * 10), 'x'));
    }
  }
  for (int i = 0; i < 100; ++i) {
    mem.Insert(++seq, "key" + std::to_string(i), "v");
  }
  EXPECT_EQ(mem.ApproximateMemoryUsage(), base);
}

}  // namespace
}  // namespace tracedb
