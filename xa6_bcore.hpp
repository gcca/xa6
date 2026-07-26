#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <mujs.h>

#include "xa6_con.hpp"

namespace xa6 {

[[nodiscard]] static void* js_alloc(void* memctx, void* ptr, int size) {
  if (!size) {
    std::free(ptr);
    return nullptr;
  }
  return std::realloc(ptr, static_cast<std::size_t>(size));
}

namespace B {

namespace detail {

struct CLIArg {
  std::string name;
  std::vector<std::string> values;
};

using CLIArgs = std::vector<CLIArg>;

static CLIArgs ParseCLIArgs(int argc, const char* const argv[]) {
  CLIArgs args;

  for (int index = 2; index < argc; ++index) {
    if (!argv[index])
      continue;

    const std::string_view option{argv[index]};
    if (!option.starts_with("--"))
      continue;

    const std::size_t equals = option.find('=');
    if (equals == std::string_view::npos || equals == 2)
      continue;

    const std::string_view name = option.substr(2, equals - 2);
    const std::string_view value = option.substr(equals + 1);

    auto existing =
        std::find_if(args.begin(), args.end(),
                     [name](const CLIArg& arg) { return arg.name == name; });
    if (existing == args.end()) {
      args.push_back({std::string{name}, {}});
      existing = std::prev(args.end());
    }
    existing->values.emplace_back(value);
  }

  return args;
}

}  // namespace detail

static void create_cli_args(js_State* J, int argc, const char* const argv[]) {
  auto* args = new detail::CLIArgs(detail::ParseCLIArgs(argc, argv));
  if (setjmp(*static_cast<jmp_buf*>(js_savetry(J)))) {
    delete args;
    js_throw(J);
  }

  js_newobject(J);
  for (std::size_t arg_i = 0; arg_i < args->size(); ++arg_i) {
    const detail::CLIArg& arg = (*args)[arg_i];
    js_newarray(J);
    for (std::size_t val_i = 0; val_i < arg.values.size(); ++val_i) {
      const std::string& val = arg.values[val_i];
      js_pushlstring(J, val.data(), static_cast<int>(val.size()));
      js_setindex(J, -2, static_cast<int>(val_i));
    }
    js_setproperty(J, -2, arg.name.c_str());
  }
  js_setglobal(J, "args");

  js_endtry(J);
  delete args;
}

static void print(js_State* J) {
  int top = js_gettop(J);
  for (int i = 1; i < top; i++)
    std::print("{}", js_tostring(J, i));
  js_pushundefined(J);
}

static void query(js_State* J) {
  const int top = js_gettop(J);

  if (top != 3)
    js_typeerror(J, "query expects exactly 2 args, but got %d", top - 1);
  if (!js_isstring(J, 1))
    js_typeerror(J, "query expects a string arg");
  if (!js_iscallable(J, 2))
    js_typeerror(J, "query expects a function as its second arg");

  auto* ddb = static_cast<DuckDB*>(js_currentfunctiondata(J));
  const char* sql = js_tostring(J, 1);
  char errmsg[1024] = {};

  try {
    ddb->Query(J, sql, 2);
    return;
  } catch (const std::exception& error) {
    std::snprintf(errmsg, sizeof(errmsg), "%s", error.what());
  } catch (...) {
    std::snprintf(errmsg, sizeof(errmsg), "%s", "Unknown DuckDB error");
  }

  js_error(J, "DuckDB: %s", errmsg);
}

}  // namespace B

}  // namespace xa6
