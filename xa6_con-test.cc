#include "xa6_con.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <mujs.h>

namespace {

using xa6::DuckDB;

class ScopedDuckDBPath {
 public:
  explicit ScopedDuckDBPath(const char* path) {
    if (const char* original = std::getenv(kName))
      original_ = original;
    Set(path);
  }

  ~ScopedDuckDBPath() noexcept {
    if (original_)
      (void)::setenv(kName, original_->c_str(), 1);
    else
      (void)::unsetenv(kName);
  }

  ScopedDuckDBPath(const ScopedDuckDBPath&) = delete;
  ScopedDuckDBPath& operator=(const ScopedDuckDBPath&) = delete;

 private:
  static constexpr const char* kName = "XA6_CON_DDB_PATH";

  static void Set(const char* path) {
    const int result = path ? ::setenv(kName, path, 1) : ::unsetenv(kName);
    if (result != 0)
      throw std::system_error(errno, std::generic_category(), kName);
  }

  std::optional<std::string> original_;
};

class UniqueTempDirectory {
 public:
  UniqueTempDirectory() {
    const auto pattern =
        (std::filesystem::path(testing::TempDir()) / "xa6-duckdb-test-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');

    const char* created = ::mkdtemp(writable.data());
    if (!created)
      throw std::system_error(errno, std::generic_category(), "mkdtemp");
    path_ = created;
  }

  ~UniqueTempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  UniqueTempDirectory(const UniqueTempDirectory&) = delete;
  UniqueTempDirectory& operator=(const UniqueTempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

class ScopedClogCapture {
 public:
  ScopedClogCapture() {
    std::clog.flush();
    original_ = std::clog.rdbuf(output_.rdbuf());
  }

  ~ScopedClogCapture() noexcept {
    std::clog.flush();
    (void)std::clog.rdbuf(original_);
  }

  ScopedClogCapture(const ScopedClogCapture&) = delete;
  ScopedClogCapture& operator=(const ScopedClogCapture&) = delete;

  [[nodiscard]] std::string str() const { return output_.str(); }

 private:
  std::ostringstream output_;
  std::streambuf* original_ = nullptr;
};

class DuckDBTest : public testing::Test {
 protected:
  void SetUp() override {
    state_ = js_newstate(nullptr, nullptr, 0);
    ASSERT_NE(state_, nullptr);
    ASSERT_EQ(js_dostring(state_, "function __xa6_noop() {}"), 0);
  }

  void TearDown() override {
    if (state_)
      js_freestate(state_);
  }

  [[nodiscard]] js_State* state() const noexcept { return state_; }

  void Execute(DuckDB& database, std::string_view sql) {
    js_getglobal(state_, "__xa6_noop");
    database.Query(state_, sql, js_gettop(state_) - 1);
    js_pop(state_, 2);
  }

  void RunQuery(DuckDB& database, std::string_view sql) {
    ASSERT_EQ(js_dostring(state_,
                          "var __xa6_rows = [];"
                          "var __xa6_push = function (row) "
                          "{ __xa6_rows.push(row); };"),
              0);
    js_getglobal(state_, "__xa6_push");
    database.Query(state_, sql, js_gettop(state_) - 1);
    js_pop(state_, 2);
    js_getglobal(state_, "__xa6_rows");
  }

  void ExpectSubmitThrows(DuckDB& database, std::string_view sql) {
    js_getglobal(state_, "__xa6_noop");
    const int top = js_gettop(state_);
    EXPECT_THROW(database.Query(state_, sql, top - 1), std::runtime_error);
    EXPECT_EQ(js_gettop(state_), top);
    js_pop(state_, 1);
  }

 private:
  ScopedDuckDBPath path_{nullptr};
  js_State* state_ = nullptr;
};

static_assert(!std::is_copy_constructible_v<DuckDB>);
static_assert(!std::is_copy_assignable_v<DuckDB>);
static_assert(!std::is_move_constructible_v<DuckDB>);
static_assert(!std::is_move_assignable_v<DuckDB>);

TEST_F(DuckDBTest, ReturnsRowsAndColumnsAsMuJSArrays) {
  DuckDB database;

  RunQuery(database,
           "SELECT * FROM (VALUES "
           "(1::INTEGER, 'alpha'::VARCHAR, true, NULL::INTEGER, 1.5::DOUBLE), "
           "(2::INTEGER, 'beta'::VARCHAR, false, 7::INTEGER, 2.5::DOUBLE)) "
           "AS values_table(id, label, enabled, optional_value, ratio) "
           "ORDER BY id");

  ASSERT_TRUE(js_isarray(state(), -1));
  ASSERT_EQ(js_getlength(state(), -1), 2);

  js_getindex(state(), -1, 0);
  ASSERT_TRUE(js_isarray(state(), -1));
  ASSERT_EQ(js_getlength(state(), -1), 5);
  js_getindex(state(), -1, 0);
  EXPECT_TRUE(js_isnumber(state(), -1));
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 1);
  js_pop(state(), 1);
  js_getindex(state(), -1, 1);
  EXPECT_TRUE(js_isstring(state(), -1));
  EXPECT_STREQ(js_tostring(state(), -1), "alpha");
  js_pop(state(), 1);
  js_getindex(state(), -1, 2);
  EXPECT_TRUE(js_isboolean(state(), -1));
  EXPECT_TRUE(js_toboolean(state(), -1));
  js_pop(state(), 1);
  js_getindex(state(), -1, 3);
  EXPECT_TRUE(js_isnull(state(), -1));
  js_pop(state(), 1);
  js_getindex(state(), -1, 4);
  EXPECT_TRUE(js_isnumber(state(), -1));
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 1.5);
  js_pop(state(), 2);

  js_getindex(state(), -1, 1);
  ASSERT_TRUE(js_isarray(state(), -1));
  ASSERT_EQ(js_getlength(state(), -1), 5);
  js_getindex(state(), -1, 0);
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 2);
  js_pop(state(), 1);
  js_getindex(state(), -1, 1);
  EXPECT_STREQ(js_tostring(state(), -1), "beta");
  js_pop(state(), 1);
  js_getindex(state(), -1, 2);
  EXPECT_FALSE(js_toboolean(state(), -1));
  js_pop(state(), 2);

  js_pop(state(), 1);
  EXPECT_EQ(js_gettop(state()), 0);
}

TEST_F(DuckDBTest, ReturnsEmptyArrayForStatementWithoutRows) {
  DuckDB database;

  RunQuery(database, "CREATE TEMP TABLE xa6_no_rows(value INTEGER)");

  ASSERT_TRUE(js_isarray(state(), -1));
  EXPECT_EQ(js_getlength(state(), -1), 0);
  js_pop(state(), 1);
}

TEST_F(DuckDBTest, KeepsConnectionStateAcrossQueries) {
  DuckDB database;

  Execute(database, "CREATE TABLE xa6_items(id INTEGER, label VARCHAR)");
  Execute(database, "INSERT INTO xa6_items VALUES (1, 'alpha'), (2, 'beta')");
  RunQuery(database,
           "SELECT count(*), string_agg(label, ',' ORDER BY id) "
           "FROM xa6_items");

  ASSERT_EQ(js_getlength(state(), -1), 1);
  js_getindex(state(), -1, 0);
  ASSERT_EQ(js_getlength(state(), -1), 2);
  js_getindex(state(), -1, 0);
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 2);
  js_pop(state(), 1);
  js_getindex(state(), -1, 1);
  EXPECT_STREQ(js_tostring(state(), -1), "alpha,beta");
  js_pop(state(), 3);
}

TEST_F(DuckDBTest, HonorsStringViewBounds) {
  DuckDB database;
  const std::string source =
      "SELECT 'bounded-view-ok' AS marker; SELEC invalid";
  const std::string_view query{source.data(), source.find(';')};

  RunQuery(database, query);

  ASSERT_EQ(js_getlength(state(), -1), 1);
  js_getindex(state(), -1, 0);
  js_getindex(state(), -1, 0);
  EXPECT_STREQ(js_tostring(state(), -1), "bounded-view-ok");
  js_pop(state(), 3);
}

TEST_F(DuckDBTest, ReportsSqlErrorsAndRemainsUsable) {
  DuckDB database;

  ExpectSubmitThrows(database, "SELEC invalid");

  RunQuery(database, "SELECT 'recovered-after-error' AS marker");
  js_getindex(state(), -1, 0);
  js_getindex(state(), -1, 0);
  EXPECT_STREQ(js_tostring(state(), -1), "recovered-after-error");
  js_pop(state(), 3);
}

TEST_F(DuckDBTest, KeepsInMemoryDatabasesIndependent) {
  DuckDB first;
  DuckDB second;

  Execute(first, "CREATE TABLE private_table(value INTEGER)");
  Execute(first, "INSERT INTO private_table VALUES (42)");

  ExpectSubmitThrows(second, "SELECT * FROM private_table");
  RunQuery(first, "SELECT value FROM private_table");
  js_getindex(state(), -1, 0);
  js_getindex(state(), -1, 0);
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 42);
  js_pop(state(), 3);
}

TEST_F(DuckDBTest, PreservesValuesOutsideMuJSNumberPrecision) {
  DuckDB database;

  RunQuery(database,
           "SELECT 9007199254740992::BIGINT, 12.34::DECIMAL(4, 2), "
           "DATE '2026-07-29'");

  js_getindex(state(), -1, 0);
  ASSERT_EQ(js_getlength(state(), -1), 3);
  js_getindex(state(), -1, 0);
  EXPECT_TRUE(js_isstring(state(), -1));
  EXPECT_STREQ(js_tostring(state(), -1), "9007199254740992");
  js_pop(state(), 1);
  js_getindex(state(), -1, 1);
  EXPECT_TRUE(js_isstring(state(), -1));
  EXPECT_STREQ(js_tostring(state(), -1), "12.34");
  js_pop(state(), 1);
  js_getindex(state(), -1, 2);
  EXPECT_TRUE(js_isstring(state(), -1));
  EXPECT_STREQ(js_tostring(state(), -1), "2026-07-29");
  js_pop(state(), 3);
}

TEST_F(DuckDBTest, WarnsForEachDefaultStringConversion) {
  DuckDB database;
  ScopedClogCapture warnings;

  RunQuery(database,
           "SELECT 42::INTEGER, 12.34::DECIMAL(4, 2), DATE '2026-07-29'");

  EXPECT_EQ(warnings.str(),
            "WARN: Converting DuckDB type DECIMAL(4,2) to string.\n"
            "WARN: Converting DuckDB type DATE to string.\n");
  js_pop(state(), 1);
}

TEST_F(DuckDBTest, InvokesCallbackPerRowWithIndexAndReturnsCount) {
  DuckDB database;

  ASSERT_EQ(js_dostring(state(),
                        "var qe_ids = []; "
                        "function qe_cb(row, index) { "
                        "  qe_ids.push([index, row[0], row[1]]); }"),
            0);

  js_getglobal(state(), "qe_cb");
  const int callback_index = js_gettop(state()) - 1;
  database.Query(state(),
                 "SELECT * FROM (VALUES "
                 "(10::INTEGER, 'a'::VARCHAR), "
                 "(20::INTEGER, 'b'::VARCHAR), "
                 "(30::INTEGER, 'c'::VARCHAR)) AS t(id, label) ORDER BY id",
                 callback_index);

  ASSERT_TRUE(js_isnumber(state(), -1));
  EXPECT_DOUBLE_EQ(js_tonumber(state(), -1), 3);
  js_pop(state(), 2);

  ASSERT_EQ(js_dostring(state(),
                        "var qe_ok = qe_ids.length === 3 "
                        "&& qe_ids[0][0] === 0 && qe_ids[0][1] === 10 "
                        "&& qe_ids[0][2] === 'a' "
                        "&& qe_ids[2][0] === 2 && qe_ids[2][1] === 30 "
                        "&& qe_ids[2][2] === 'c';"),
            0);
  js_getglobal(state(), "qe_ok");
  EXPECT_TRUE(js_toboolean(state(), -1));
  js_pop(state(), 1);
  EXPECT_EQ(js_gettop(state()), 0);
}

TEST_F(DuckDBTest, LeavesStackUnchangedOnSubmitError) {
  DuckDB database;

  js_pushnumber(state(), 0);
  const int callback_index = js_gettop(state()) - 1;
  const int original_top = js_gettop(state());

  EXPECT_THROW(database.Query(state(), "SELEC invalid", callback_index),
               std::runtime_error);
  EXPECT_EQ(js_gettop(state()), original_top);
  js_pop(state(), 1);
}

TEST_F(DuckDBTest, OpensFileDatabaseReadOnly) {
  UniqueTempDirectory directory;
  const std::string path = (directory.path() / "state.duckdb").string();
  ScopedDuckDBPath database_path{path.c_str()};

  EXPECT_THROW({ DuckDB reader; }, std::exception);
  EXPECT_FALSE(std::filesystem::exists(path));
}

}  // namespace
