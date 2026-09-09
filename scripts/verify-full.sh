#!/usr/bin/env bash
# Slower pre-PR / milestone verification:
#   fast suite -> exhaustive sanitizer sweep -> Release build and suite.
# Release is built without sanitizers; never quote sanitizer builds as throughput numbers.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"

step() { printf '\n########## %s\n' "$1"; }

step "1/3 fast verification"
./scripts/verify.sh

step "2/3 exhaustive sanitizer sweep (EVENTLOG_SLOW_TESTS=ON)"
cmake -S "$ROOT" -B build/debug-asan-slow \
  -DCMAKE_BUILD_TYPE=Debug -DEVENTLOG_SANITIZERS=ON -DEVENTLOG_SLOW_TESTS=ON >/dev/null
cmake --build build/debug-asan-slow --parallel
ctest --test-dir build/debug-asan-slow --output-on-failure

step "3/3 Release build and suite"
cmake -S "$ROOT" -B build/release \
  -DCMAKE_BUILD_TYPE=Release -DEVENTLOG_SANITIZERS=OFF -DEVENTLOG_SLOW_TESTS=ON >/dev/null
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure

step "verify-full.sh PASS"
