#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <mujs.h>

#define main xa6_cli_main
#include "xa6.cc"
#undef main

namespace {

void CaptureReport(js_State* state, const char* message) {
  auto& reports = *static_cast<std::vector<std::string>*>(js_getcontext(state));
  reports.emplace_back(message ? message : "");
}

bool ContainsReport(const std::vector<std::string>& reports,
                    std::string_view expected) {
  for (const auto& report : reports) {
    if (report.find(expected) != std::string::npos)
      return true;
  }
  return false;
}

TEST(Xa6CliTest, PrintsUsageWhenScriptIsMissing) {
  char executable[] = "xa6";
  char* argv[] = {executable, nullptr};

  testing::internal::CaptureStderr();
  const int status = xa6_cli_main(1, argv);
  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(status, EXIT_FAILURE);
  EXPECT_EQ(output,
            "ERROR: No file specified.\n"
            "Usage: xa6 <script.js> [--name=value ...]\n");
}

class EjsTest : public testing::Test {
 protected:
  void SetUp() override {
    state_ = js_newstate(xa6::js_alloc, nullptr, 0);
    ASSERT_NE(state_, nullptr);

    js_setcontext(state_, &reports_);
    js_setreport(state_, CaptureReport);
    js_newcfunction(state_, xa6::B::print, "print", 0);
    js_setglobal(state_, "print");
    js_newcfunctionx(state_, xa6::B::query, "query", 1, &database_, nullptr);
    js_setglobal(state_, "query");
  }

  void TearDown() override {
    if (!state_)
      return;
    js_gc(state_, 0);
    js_freestate(state_);
  }

  int Evaluate(std::string_view source) {
    const std::string script{source};
    return js_dostring(state_, script.c_str());
  }

  double GlobalNumber(const char* name) {
    js_getglobal(state_, name);
    const double result = js_tonumber(state_, -1);
    js_pop(state_, 1);
    return result;
  }

  bool GlobalIsUndefined(const char* name) {
    js_getglobal(state_, name);
    const bool result = js_isundefined(state_, -1);
    js_pop(state_, 1);
    return result;
  }

  bool GlobalBoolean(const char* name) {
    js_getglobal(state_, name);
    const bool result = js_toboolean(state_, -1);
    js_pop(state_, 1);
    return result;
  }

  js_State* state_ = nullptr;
  std::vector<std::string> reports_;
  xa6::DuckDB database_;
};

TEST_F(EjsTest, EvaluatesJavaScriptStrings) {
  ASSERT_EQ(Evaluate("var embedded_result = 6 * 7 + 0.5;"), 0);

  EXPECT_DOUBLE_EQ(GlobalNumber("embedded_result"), 42.5);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, KeepsStateBetweenScriptStrings) {
  ASSERT_EQ(Evaluate("var embedded_counter = 40;"), 0);
  ASSERT_EQ(Evaluate("embedded_counter += 2;"), 0);

  EXPECT_DOUBLE_EQ(GlobalNumber("embedded_counter"), 42);
}

TEST_F(EjsTest, ExposesPrintToJavaScript) {
  testing::internal::CaptureStdout();
  const int status =
      Evaluate(R"(var print_result = print("answer=", 6 * 7, "\n");)");
  const std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(status, 0);
  EXPECT_EQ(output, "answer=42\n");
  EXPECT_TRUE(GlobalIsUndefined("print_result"));
}

TEST_F(EjsTest, ReportsSyntaxErrors) {
  EXPECT_NE(Evaluate("var broken = ;"), 0);

  EXPECT_FALSE(reports_.empty());
  EXPECT_TRUE(ContainsReport(reports_, "SyntaxError"));
}

TEST_F(EjsTest, RecoversAfterRuntimeErrors) {
  EXPECT_NE(Evaluate(R"(throw new Error("embedded failure");)"), 0);
  EXPECT_TRUE(ContainsReport(reports_, "embedded failure"));

  reports_.clear();
  ASSERT_EQ(Evaluate("var recovered_value = 21 * 2;"), 0);
  EXPECT_DOUBLE_EQ(GlobalNumber("recovered_value"), 42);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, DeliversDuckDBRowsToCallbackWithIndex) {
  ASSERT_EQ(Evaluate(R"JS(
        var query_rows = [];
        var query_indices = [];
        var query_row_count = query(
          "SELECT * FROM (VALUES "
          + "(1::INTEGER, 'alpha'::VARCHAR, true, NULL::INTEGER), "
          + "(2::INTEGER, 'beta'::VARCHAR, false, 7::INTEGER)) "
          + "AS values_table(id, label, enabled, optional_value) "
          + "ORDER BY id",
          function (row, index) {
            query_rows.push(row);
            query_indices.push(index);
          });
        var query_rows_are_arrays =
          query_rows[0] instanceof Array
          && query_rows[1] instanceof Array;
        var query_values_match =
          query_row_count === 2
          && query_rows.length === 2
          && query_rows[0].length === 4
          && query_rows[0][0] === 1
          && query_rows[0][1] === "alpha"
          && query_rows[0][2] === true
          && query_rows[0][3] === null
          && query_rows[1][0] === 2
          && query_rows[1][1] === "beta"
          && query_rows[1][2] === false
          && query_rows[1][3] === 7
          && query_indices[0] === 0
          && query_indices[1] === 1;
      )JS"),
            0);

  EXPECT_TRUE(GlobalBoolean("query_rows_are_arrays"));
  EXPECT_TRUE(GlobalBoolean("query_values_match"));
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, ReportsDuckDBErrorsAndRecovers) {
  EXPECT_NE(Evaluate(R"JS(query("SELEC invalid", function () {});)JS"), 0);
  EXPECT_TRUE(ContainsReport(reports_, "DuckDB"));

  reports_.clear();
  ASSERT_EQ(Evaluate(R"JS(
        var query_recovered;
        query("SELECT 6 * 7", function (row) { query_recovered = row[0]; });
      )JS"),
            0);
  EXPECT_DOUBLE_EQ(GlobalNumber("query_recovered"), 42);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, InvokesQueryCallbackZeroTimesForResultWithoutRows) {
  ASSERT_EQ(Evaluate(R"JS(
        var empty_callback_calls = 0;
        var empty_callback_count = query(
          "CREATE OR REPLACE TEMP TABLE xa6_js_cb_no_rows(value INTEGER)",
          function () { empty_callback_calls += 1; });
      )JS"),
            0);

  EXPECT_DOUBLE_EQ(GlobalNumber("empty_callback_calls"), 0);
  EXPECT_DOUBLE_EQ(GlobalNumber("empty_callback_count"), 0);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, RejectsReentrantQueryFromInsideCallbackAndRecovers) {
  EXPECT_NE(Evaluate(R"JS(
        query("SELECT 1",
              function () { query("SELECT 2", function () {}); });
      )JS"),
            0);
  EXPECT_TRUE(ContainsReport(reports_, "DuckDB"));

  reports_.clear();
  ASSERT_EQ(Evaluate(R"JS(
        var reentry_recovered;
        query("SELECT 6 * 7", function (row) { reentry_recovered = row[0]; });
      )JS"),
            0);
  EXPECT_DOUBLE_EQ(GlobalNumber("reentry_recovered"), 42);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, PropagatesErrorsThrownInsideCallbackAndRecovers) {
  EXPECT_NE(Evaluate(R"JS(
        query("SELECT 1", function () { throw new Error("callback boom"); });
      )JS"),
            0);
  EXPECT_TRUE(ContainsReport(reports_, "callback boom"));

  reports_.clear();
  ASSERT_EQ(Evaluate(R"JS(
        var after_callback_throw;
        query("SELECT 6 * 7",
              function (row) { after_callback_throw = row[0]; });
      )JS"),
            0);
  EXPECT_DOUBLE_EQ(GlobalNumber("after_callback_throw"), 42);
  EXPECT_TRUE(reports_.empty());
}

TEST_F(EjsTest, ExposesCommandLineArgumentsToJavaScriptStrings) {
  const char* argv[] = {
      "xa6", "script.js", "--arg=value1", "--mode=test", "--arg=value2",
  };
  xa6::B::create_cli_args(state_, 5, argv);

  ASSERT_EQ(Evaluate(R"JS(
        var cli_arguments_match =
          args.arg instanceof Array
          && args.arg.length === 2
          && args.arg[0] === "value1"
          && args.arg[1] === "value2"
          && args.mode instanceof Array
          && args.mode.length === 1
          && args.mode[0] === "test";
      )JS"),
            0);

  EXPECT_TRUE(GlobalBoolean("cli_arguments_match"));
  EXPECT_TRUE(reports_.empty());
}

}  // namespace
