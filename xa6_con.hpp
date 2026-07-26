#pragma once

#include <cstddef>
#include <string_view>

struct js_State;

namespace xa6 {

class DuckDB {
 public:
  DuckDB();
  ~DuckDB();

  DuckDB(const DuckDB&) = delete;
  DuckDB(DuckDB&&) = delete;
  DuckDB& operator=(const DuckDB&) = delete;
  DuckDB& operator=(DuckDB&&) = delete;

  void Query(js_State* J, std::string_view sql, int callback_index);

 private:
  struct Impl;
  static constexpr std::size_t kImplSize = 64;
  [[nodiscard]] Impl& impl() noexcept;
  alignas(std::max_align_t) std::byte impl_storage_[kImplSize];
  bool querying_ = false;
};

}  // namespace xa6
