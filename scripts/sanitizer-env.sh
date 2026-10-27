#!/usr/bin/env bash
# Sourced by the verify scripts. Chooses sanitizer runtime options for the host.
# Not executable on its own.

# LeakSanitizer only exists on Linux; asking for it elsewhere makes ASan abort on startup.
if [ -z "${ASAN_OPTIONS:-}" ]; then
  case "$(uname -s)" in
    Linux) ASAN_OPTIONS="detect_leaks=1" ;;
    *) ASAN_OPTIONS="detect_leaks=0" ;;
  esac
fi
export ASAN_OPTIONS
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
