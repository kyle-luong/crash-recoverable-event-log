#!/usr/bin/env bash
# Slower pre-milestone verification:
#   fast suite -> exhaustive sanitizer sweep -> Release build and suite.
# Release is built without sanitizers; never quote sanitizer builds as throughput numbers.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
# May point outside the source tree; see scripts/verify.sh.
BUILD_ROOT="${BUILD_ROOT:-build}"
export BUILD_ROOT

# shellcheck source=scripts/sanitizer-env.sh
. "$ROOT/scripts/sanitizer-env.sh"

step() { printf '\n########## %s\n' "$1"; }

step "1/3 fast verification"
./scripts/verify.sh

step "2/3 exhaustive sanitizer sweep (EVENTLOG_SLOW_TESTS=ON)"
cmake -S "$ROOT" -B "$BUILD_ROOT/debug-asan-slow" \
  -DCMAKE_BUILD_TYPE=Debug -DEVENTLOG_SANITIZERS=ON -DEVENTLOG_SLOW_TESTS=ON >/dev/null
cmake --build "$BUILD_ROOT/debug-asan-slow" --parallel
ctest --test-dir "$BUILD_ROOT/debug-asan-slow" --output-on-failure

step "3/3 Release build and suite"
cmake -S "$ROOT" -B "$BUILD_ROOT/release" \
  -DCMAKE_BUILD_TYPE=Release -DEVENTLOG_SANITIZERS=OFF -DEVENTLOG_SLOW_TESTS=ON >/dev/null
cmake --build "$BUILD_ROOT/release" --parallel
ctest --test-dir "$BUILD_ROOT/release" --output-on-failure

step "verify-full.sh PASS"
