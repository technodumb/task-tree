// `tt` — a thin entry point around tt::cli::run so the same code path is exercised by the
// binary and by tests/cli_tests.cpp (which calls run() in-process). See src/cli/Cli.cpp.
#include <iostream>
#include <string>
#include <vector>

#include "cli/Cli.hpp"

int main(int argc, char** argv) {
    return tt::cli::run(std::vector<std::string>(argv + 1, argv + argc), std::cout, std::cerr);
}
