#!/bin/sh
# A stand-in writer that claims an acknowledgment it never earned.
#
# The supervisor must catch this. Without it, "Violations: 0" would be unfalsifiable from
# outside the process: the oracle's own negative controls live in tests/test_crash.cpp,
# but nothing else exercises the path from a detected violation to a nonzero exit code.
#
# Spawned exactly as the real child is: $1 is the subcommand, $2 the log path, $3 the
# scenario spec. It creates an empty log, then reports one durable record.
set -eu
: > "$2"
printf 'ack 1 1\ndone 1\n'
