#include "eventlog/cli.h"

#include <iostream>
#include <string_view>

namespace eventlog {
namespace {

// Placeholder command surface for the CPP-00 scaffold. The commands are listed so the
// intended interface is visible, but none of them are implemented yet and none of them
// pretend to be: every one exits with a usage error naming its milestone.
void PrintUsage(std::ostream& out) {
  out << R"(eventlog - crash-safe append-only event log (scaffold)

Usage:
  eventlog append  --file PATH --records N --payload-bytes B   [milestone M1]
  eventlog verify  --file PATH                                 [milestone M1]
  eventlog recover --file PATH [--truncate-tail]               [milestone M1]
  eventlog crash-test --runs N --seed S --workdir DIR          [milestone M3]
  eventlog bench   --records N --trials T --out PATH           [milestone M4]
  eventlog --help | --version

No log logic exists yet. This build only proves the toolchain, warnings, sanitizers,
tests, and verification scripts work. See docs/PLAN.md.
)";
}

}  // namespace

int RunCli(int argc, char** argv) {
  if (argc >= 2) {
    const std::string_view command(argv[1]);
    if (command == "--help" || command == "-h" || command == "help") {
      PrintUsage(std::cout);
      return kExitOk;
    }
    if (command == "--version") {
      std::cout << "eventlog 0.0.1 (scaffold; no record format implemented)\n";
      return kExitOk;
    }
    std::cerr << "error: \"" << command << "\" is not implemented in the scaffold\n\n";
  }
  PrintUsage(std::cerr);
  return kExitUsage;
}

}  // namespace eventlog
