#!/usr/bin/env bash
# Sourced by the verify scripts. Chooses sanitizer runtime options for the host.
# Not executable on its own.
#
# A pre-set ASAN_OPTIONS is respected, so an occasional deeper run can add options
# without editing this file.
#
# strict_string_checks is cheap here and catches an unterminated or out-of-bounds string
# operation. Deliberately NOT enabled by default:
#
#   detect_stack_use_after_return=1
#
# It catches a pointer into a frame that has already gone, which is a real bug class, but
# in a Debug build nothing inlines, so every call allocates a fake stack frame. Measured
# on Linux: the exhaustive recovery sweep goes from 10.7 s to over 420 s -- more than 39x
# -- because that sweep performs tens of millions of trivial container operations. The
# code it would instrument returns Result<T> by value everywhere rather than references
# to locals, so the margin is small and the gate that runs before every commit is where
# the cost lands. Enable it for a one-off run instead:
#
#   ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1" ./scripts/verify.sh

# LeakSanitizer only exists on Linux; asking for it elsewhere makes ASan abort on startup.
if [ -z "${ASAN_OPTIONS:-}" ]; then
  case "$(uname -s)" in
    Linux) ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1" ;;
    *) ASAN_OPTIONS="detect_leaks=0:strict_string_checks=1" ;;
  esac
fi
export ASAN_OPTIONS
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
