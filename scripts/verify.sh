#!/usr/bin/env bash
# Canonical fast verification. Must pass locally and in CI before any commit.
# Format check -> -Werror build with ASan+UBSan -> full CTest suite.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
BUILD_DIR="${BUILD_DIR:-build/debug-asan}"

step() { printf '\n==> %s\n' "$1"; }

# --- clang-format -------------------------------------------------------------
# Resolved from PATH, then from the Xcode command line tools (macOS has no
# clang-format on PATH by default).
find_clang_format() {
  if command -v clang-format >/dev/null 2>&1; then command -v clang-format; return 0; fi
  if command -v xcrun >/dev/null 2>&1; then xcrun -f clang-format 2>/dev/null && return 0; fi
  return 1
}

# Portable across the bash 3.2 that macOS ships (no mapfile / no readarray).
list_sources() {
  find include src tests -type f \( -name '*.cpp' -o -name '*.h' \) | sort
}

step "Format check"
if CLANG_FORMAT="$(find_clang_format)"; then
  echo "using $CLANG_FORMAT ($("$CLANG_FORMAT" --version))"
  list_sources | tr '\n' '\0' | xargs -0 "$CLANG_FORMAT" --style=file --dry-run -Werror
  echo "format OK ($(list_sources | wc -l | tr -d ' ') files)"
else
  echo "SKIP: clang-format not found; install it to enforce formatting" >&2
fi

# --- build --------------------------------------------------------------------
step "Configure ($BUILD_DIR, Debug, sanitizers on)"
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEVENTLOG_SANITIZERS=ON \
  -DCMAKE_COLOR_DIAGNOSTICS=ON >/dev/null

step "Build"
cmake --build "$BUILD_DIR" --parallel

# --- test ---------------------------------------------------------------------
step "Test (ctest)"
# LeakSanitizer only exists on Linux; asking for it on macOS makes ASan abort on startup.
if [ -z "${ASAN_OPTIONS:-}" ]; then
  case "$(uname -s)" in
    Linux) ASAN_OPTIONS="detect_leaks=1" ;;
    *) ASAN_OPTIONS="detect_leaks=0" ;;
  esac
fi
export ASAN_OPTIONS
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
echo "ASAN_OPTIONS=$ASAN_OPTIONS"
ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel 4

step "verify.sh PASS"
