#pragma once
// Headless CLI over the task store. `run(args, out, err)` is the whole surface: the `tt`
// executable is a thin main() around it, and tests/cli_tests.cpp drives it in-process
// against a temp store (XDG_DATA_HOME / --store). No windowing deps — it links tt_core +
// tt_io only, so it builds and runs wherever the toolchain does, app or no app.

#include <iosfwd>
#include <string>
#include <vector>

namespace tt::cli {

// Exit codes are a contract, documented in `--help`: a caller must be able to tell the
// failure classes apart. Keep this enum and the `--help` text (Cli.cpp) in sync.
enum Exit {
    kOk         = 0,
    kUsage      = 1,  // unknown command, bad flags, or an operation the model refused (e.g. a cycle)
    kNotFound   = 2,  // an id or query resolved to no live task
    kAmbiguous  = 3,  // a text query matched more than one live task
    kUnreadable = 4,  // the store is missing, or exists but could not be read (run the app to recover)
    kTooNew     = 5,  // the store's schema is newer than this build understands
    kBusy       = 6,  // the store could not be written (locked past the busy timeout, or unwritable)
};

// Run one invocation. `args` is argv WITHOUT the program name. Parseable output goes to
// `out`; every diagnostic goes to `err`, so stdout stays clean under `--json`.
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace tt::cli
