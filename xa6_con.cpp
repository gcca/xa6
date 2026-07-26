#include "xa6_con.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <duckdb.hpp>
#include <mujs.h>

namespace xa6 {

namespace {

struct ScalarCell {
  enum class Kind { kNull, kBoolean, kNumber, kString };
  Kind kind = Kind::kNull;
  bool boolean = false;
  double number = 0;
};

ScalarCell ExtractScalar(const duckdb::Value& value, std::string& scratch) {
  ScalarCell cell;
  if (value.IsNull())
    return cell;

  using duckdb::LogicalTypeId;
  switch (value.type().id()) {
    case LogicalTypeId::BOOLEAN:
      cell.kind = ScalarCell::Kind::kBoolean;
      cell.boolean = value.GetValue<bool>();
      return cell;

    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
      cell.kind = ScalarCell::Kind::kNumber;
      cell.number = value.GetValue<double>();
      return cell;

    case LogicalTypeId::BIGINT: {
      constexpr std::int64_t kMaxSafeInteger = 9007199254740991;
      const std::int64_t integer = value.GetValue<std::int64_t>();
      if (integer >= -kMaxSafeInteger && integer <= kMaxSafeInteger) {
        cell.kind = ScalarCell::Kind::kNumber;
        cell.number = static_cast<double>(integer);
        return cell;
      }
      break;
    }

    case LogicalTypeId::UBIGINT: {
      constexpr std::uint64_t kMaxSafeInteger = 9007199254740991;
      const std::uint64_t integer = value.GetValue<std::uint64_t>();
      if (integer <= kMaxSafeInteger) {
        cell.kind = ScalarCell::Kind::kNumber;
        cell.number = static_cast<double>(integer);
        return cell;
      }
      break;
    }

    default:
      break;
  }

  scratch = value.ToString();
  cell.kind = ScalarCell::Kind::kString;
  return cell;
}

void PushChunkCell(js_State* J,
                   duckdb::DataChunk& chunk,
                   duckdb::idx_t column,
                   duckdb::idx_t row,
                   std::string& scratch) {
  ScalarCell cell;
  {
    const duckdb::Value value = chunk.GetValue(column, row);
    cell = ExtractScalar(value, scratch);
  }

  switch (cell.kind) {
    case ScalarCell::Kind::kNull:
      js_pushnull(J);
      break;
    case ScalarCell::Kind::kBoolean:
      js_pushboolean(J, cell.boolean);
      break;
    case ScalarCell::Kind::kNumber:
      js_pushnumber(J, cell.number);
      break;
    case ScalarCell::Kind::kString:
      js_pushlstring(J, scratch.data(), static_cast<int>(scratch.size()));
      break;
  }
}

void PushRowArray(js_State* J,
                  duckdb::DataChunk& chunk,
                  duckdb::idx_t row,
                  duckdb::idx_t columns,
                  std::string& scratch) {
  js_newarray(J);
  for (duckdb::idx_t column = 0; column < columns; ++column) {
    PushChunkCell(J, chunk, column, row, scratch);
    js_setindex(J, -2, static_cast<int>(column));
  }
}

struct StreamState {
  explicit StreamState(duckdb::unique_ptr<duckdb::QueryResult> result)
      : result{std::move(result)} {}

  duckdb::unique_ptr<duckdb::QueryResult> result;
  duckdb::unique_ptr<duckdb::DataChunk> chunk;
  std::string scratch;
};

duckdb::unique_ptr<duckdb::QueryResult> StartQuery(
    duckdb::Connection& connection,
    std::string_view sql) {
  auto result = connection.SendQuery(std::string{sql});
  if (!result)
    throw std::runtime_error("DuckDB did not return a query result.");
  if (result->HasError())
    throw std::runtime_error(result->GetError());
  return result;
}

duckdb::DuckDB OpenDatabase(const char* path) {
  if (!path)
    return duckdb::DuckDB(nullptr);

  duckdb::DBConfig config;
  config.options.access_mode = duckdb::AccessMode::READ_ONLY;
  return duckdb::DuckDB(path, &config);
}

}  // namespace

struct DuckDB::Impl {
  explicit Impl(const char* path)
      : database{OpenDatabase(path)}, connection{database} {}

  duckdb::DuckDB database;
  duckdb::Connection connection;
  std::unique_ptr<StreamState> stream_state;
};

DuckDB::DuckDB() {
  static_assert(sizeof(Impl) <= kImplSize,
                "DuckDB implementation storage is too small.");
  static_assert(alignof(Impl) <= alignof(std::max_align_t),
                "DuckDB implementation storage is under-aligned.");

  const char* ddb_path = std::getenv("XA6_CON_DDB_PATH");
  ::new (static_cast<void*>(impl_storage_)) Impl{ddb_path};
}

DuckDB::~DuckDB() {
  impl().~Impl();
}

DuckDB::Impl& DuckDB::impl() noexcept {
  return *std::launder(reinterpret_cast<Impl*>(impl_storage_));
}

void DuckDB::Query(js_State* J, std::string_view sql, int callback_index) {
  if (querying_)
    throw std::runtime_error("query() cannot run inside a query() callback.");

  auto& state = impl().stream_state;
  state = std::make_unique<StreamState>(StartQuery(impl().connection, sql));
  if (setjmp(*static_cast<jmp_buf*>(js_savetry(J)))) {
    querying_ = false;
    state.reset();
    js_throw(J);
  }
  querying_ = true;

  try {
    std::size_t row_count = 0;

    for (;;) {
      state->chunk = state->result->Fetch();
      if (!state->chunk || state->chunk->size() == 0)
        break;

      const duckdb::idx_t columns = state->chunk->ColumnCount();
      const duckdb::idx_t rows = state->chunk->size();

      for (duckdb::idx_t row = 0; row < rows; ++row) {
        js_copy(J, callback_index);
        js_pushundefined(J);
        PushRowArray(J, *state->chunk, row, columns, state->scratch);
        js_pushnumber(J, static_cast<double>(row_count));
        js_call(J, 2);
        js_pop(J, 1);
        ++row_count;
      }
      state->chunk.reset();
    }

    js_endtry(J);
    querying_ = false;
    state.reset();
    js_pushnumber(J, static_cast<double>(row_count));
  } catch (...) {
    js_endtry(J);
    querying_ = false;
    state.reset();
    throw;
  }
}

}  // namespace xa6
