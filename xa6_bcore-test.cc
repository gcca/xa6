// clang-format off
#include "xa6_bcore.hpp"
#include "xa6_bcore.hpp"
// clang-format on

#include <gtest/gtest.h>

namespace {

TEST(Xa6BcoreTest, HeaderIsSelfContainedAndIdempotent) {
  SUCCEED();
}

class CliArgsTest : public testing::Test {
 protected:
  void SetUp() override {
    state_ = js_newstate(xa6::js_alloc, nullptr, 0);
    ASSERT_NE(state_, nullptr);
  }

  void TearDown() override {
    if (state_)
      js_freestate(state_);
  }

  js_State* state_ = nullptr;
};

TEST_F(CliArgsTest, CreatesGlobalObjectWithArraysOfValues) {
  const char* argv[] = {
      "xa6",
      "--script=not-an-argument",
      "--arg=value1",
      "--other=first",
      "--arg=value2",
      "--empty=",
      "--expr=a=b=c",
      "--toString=text",
      "--constructor=ctor",
      "not-an-option",
      "--missing-value",
      "--=missing-key",
  };

  xa6::B::create_cli_args(state_, 12, argv);

  js_getglobal(state_, "args");
  ASSERT_TRUE(js_isobject(state_, -1));
  EXPECT_FALSE(js_isarray(state_, -1));

  js_getproperty(state_, -1, "arg");
  ASSERT_TRUE(js_isarray(state_, -1));
  ASSERT_EQ(js_getlength(state_, -1), 2);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "value1");
  js_pop(state_, 1);
  js_getindex(state_, -1, 1);
  EXPECT_STREQ(js_tostring(state_, -1), "value2");
  js_pop(state_, 2);

  js_getproperty(state_, -1, "other");
  ASSERT_EQ(js_getlength(state_, -1), 1);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "first");
  js_pop(state_, 2);

  js_getproperty(state_, -1, "empty");
  ASSERT_EQ(js_getlength(state_, -1), 1);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "");
  js_pop(state_, 2);

  js_getproperty(state_, -1, "expr");
  ASSERT_EQ(js_getlength(state_, -1), 1);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "a=b=c");
  js_pop(state_, 2);

  js_getproperty(state_, -1, "toString");
  ASSERT_TRUE(js_isarray(state_, -1));
  ASSERT_EQ(js_getlength(state_, -1), 1);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "text");
  js_pop(state_, 2);

  js_getproperty(state_, -1, "constructor");
  ASSERT_TRUE(js_isarray(state_, -1));
  ASSERT_EQ(js_getlength(state_, -1), 1);
  js_getindex(state_, -1, 0);
  EXPECT_STREQ(js_tostring(state_, -1), "ctor");
  js_pop(state_, 2);

  EXPECT_FALSE(js_hasproperty(state_, -1, "script"));
  EXPECT_FALSE(js_hasproperty(state_, -1, "missing-value"));
  EXPECT_FALSE(js_hasproperty(state_, -1, ""));
  js_pop(state_, 1);
  EXPECT_EQ(js_gettop(state_), 0);
}

TEST_F(CliArgsTest, EmptyArgumentsReplaceThePreviousGlobalObject) {
  const char* original[] = {"xa6", "script.js", "--arg=stale"};
  xa6::B::create_cli_args(state_, 3, original);

  const char* replacement[] = {"xa6", "script.js"};
  xa6::B::create_cli_args(state_, 2, replacement);

  js_getglobal(state_, "args");
  ASSERT_TRUE(js_isobject(state_, -1));
  js_getproperty(state_, -1, "arg");
  EXPECT_TRUE(js_isundefined(state_, -1));
  js_pop(state_, 2);
  EXPECT_EQ(js_gettop(state_), 0);
}

}  // namespace
