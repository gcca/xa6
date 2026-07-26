#include <cstdlib>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

#include "xa6_bcore.hpp"

namespace {

void Usage() {
  std::println(std::cerr, "Usage: xa6 <script.js> [--name=value ...]");
}

std::string ReadStdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return std::move(buffer).str();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::println(std::cerr, "ERROR: No file specified.");
    Usage();
    return EXIT_FAILURE;
  }

  xa6::DuckDB ddb;
  js_State* J = js_newstate(xa6::js_alloc, nullptr, 0);
  if (!J) {
    std::println(std::cerr, "ERROR: Could not create new State.");
    return EXIT_FAILURE;
  }

  xa6::B::create_cli_args(J, argc, argv);
  js_newcfunction(J, xa6::B::print, "print", 0);
  js_setglobal(J, "print");
  js_newcfunctionx(J, xa6::B::query, "query", 1, &ddb, nullptr);
  js_setglobal(J, "query");

  const bool from_stdin = std::string_view{argv[1]} == "-";
  const int status = from_stdin ? js_dostring(J, ReadStdin().c_str())
                                : js_dofile(J, argv[1]);
  if (status) {
    std::println(std::cerr, "ERROR: Could not load {}: {}",
                 from_stdin ? "stdin" : argv[1], js_tostring(J, -1));
    goto failure;
  }

  js_gc(J, 0);
  js_freestate(J);

  return EXIT_SUCCESS;

failure:
  js_gc(J, 0);
  js_freestate(J);

  return EXIT_FAILURE;
}
