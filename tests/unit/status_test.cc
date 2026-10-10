#include "tracedb/status.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

namespace tracedb {
namespace {

// ---------------- Status ----------------

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_EQ(s.message(), "");
}

TEST(StatusTest, OkFactoryIsOk) {
  EXPECT_TRUE(Status::OK().ok());
}

TEST(StatusTest, FactoriesSetCodeAndMessage) {
  Status nf = Status::NotFound("file 7");
  EXPECT_FALSE(nf.ok());
  EXPECT_TRUE(nf.IsNotFound());
  EXPECT_EQ(nf.code(), Status::Code::kNotFound);
  EXPECT_EQ(nf.message(), "file 7");

  Status c = Status::Corruption("bad crc");
  EXPECT_FALSE(c.ok());
  EXPECT_TRUE(c.IsCorruption());
  EXPECT_FALSE(c.IsIOError());
  EXPECT_EQ(c.message(), "bad crc");

  Status ia = Status::InvalidArgument("empty dir");
  EXPECT_TRUE(ia.IsInvalidArgument());

  Status io = Status::IOError("disk full");
  EXPECT_TRUE(io.IsIOError());
  EXPECT_FALSE(io.IsCorruption());
}

TEST(StatusTest, ToString) {
  EXPECT_EQ(Status::OK().ToString(), "OK");
  EXPECT_EQ(Status::NotFound("file 7").ToString(), "Not found: file 7");
  EXPECT_EQ(Status::Corruption("bad crc in block 3").ToString(),
            "Corruption: bad crc in block 3");
  EXPECT_EQ(Status::InvalidArgument("x").ToString(), "Invalid argument: x");
  EXPECT_EQ(Status::IOError("disk full").ToString(), "IO error: disk full");
}

TEST(StatusTest, CopyPreservesError) {
  Status a = Status::Corruption("bad magic");
  Status b = a;
  EXPECT_TRUE(b.IsCorruption());
  EXPECT_EQ(b.message(), "bad magic");
}

// ---------------- Result ----------------

TEST(ResultTest, HoldsValue) {
  Result<int> r = 42;
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.status().ok());
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
}

TEST(ResultTest, HoldsError) {
  Result<int> r = Status::IOError("disk");
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.status().IsIOError());
  EXPECT_EQ(r.status().message(), "disk");
}

TEST(ResultTest, MoveOnlyValueCanBeMovedOut) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(7);
  ASSERT_TRUE(r.ok());
  std::unique_ptr<int> p = std::move(r).value();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 7);
}

struct Base {
  virtual ~Base() = default;
  virtual int Id() const { return 1; }
};
struct Derived : Base {
  int Id() const override { return 2; }
};

TEST(ResultTest, ConvertsDerivedPointerToBase) {
  Result<std::unique_ptr<Base>> r = std::make_unique<Derived>();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ((*r)->Id(), 2);
}

TEST(ResultTest, OptionalNullopt) {
  // A missing key is an OK result holding nullopt, not an error.
  Result<std::optional<int>> r = std::nullopt;
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.value().has_value());
}

TEST(ResultTest, ArrowOperator) {
  Result<std::string> r = std::string("hello");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->size(), 5u);
}

Result<int> ParsePositive(int x) {
  if (x <= 0) return Status::InvalidArgument("not positive");
  return x;
}

TEST(ResultTest, ImplicitConversionFromBothBranches) {
  Result<int> good = ParsePositive(5);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 5);

  Result<int> bad = ParsePositive(-1);
  EXPECT_FALSE(bad.ok());
  EXPECT_TRUE(bad.status().IsInvalidArgument());
}

// ---------------- TRACEDB_RETURN_IF_ERROR ----------------

int g_steps = 0;

Status Step(bool fail) {
  ++g_steps;
  return fail ? Status::IOError("step failed") : Status::OK();
}

Status RunSteps(bool fail_second) {
  TRACEDB_RETURN_IF_ERROR(Step(false));
  TRACEDB_RETURN_IF_ERROR(Step(fail_second));
  TRACEDB_RETURN_IF_ERROR(Step(false));
  return Status::OK();
}

TEST(ReturnIfErrorTest, StopsAtFirstError) {
  g_steps = 0;
  Status s = RunSteps(/*fail_second=*/true);
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(g_steps, 2);  // the third step never ran
}

TEST(ReturnIfErrorTest, RunsAllStepsOnSuccess) {
  g_steps = 0;
  EXPECT_TRUE(RunSteps(/*fail_second=*/false).ok());
  EXPECT_EQ(g_steps, 3);
}

}  // namespace
}  // namespace tracedb
