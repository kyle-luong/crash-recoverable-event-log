#ifndef EVENTLOG_CLI_H_
#define EVENTLOG_CLI_H_

namespace eventlog {

// Exit codes follow the table in docs/PRD.md section 3.
inline constexpr int kExitOk = 0;
inline constexpr int kExitIncompleteTail = 1;  // verify only
inline constexpr int kExitHardCorruption = 2;
inline constexpr int kExitHarnessFailure = 3;
inline constexpr int kExitRuntimeError = 10;
inline constexpr int kExitUsage = 64;

int RunCli(int argc, char** argv);

}  // namespace eventlog

#endif  // EVENTLOG_CLI_H_
