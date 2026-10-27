#!/usr/bin/env bash
# Linux verification. Linux is the supported platform (docs/decisions/ADR-004), so this is the
# run whose result counts as evidence in docs/STATUS.md -- a macOS pass is a convenience check.
#
# The repository is mounted READ-ONLY at /src and all build output goes to a container-local
# /work. The host tree is therefore untouched by this script, and the macOS build/ directory
# cannot collide with the Linux one.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
IMAGE="${EVENTLOG_LINUX_IMAGE:-eventlog-linux-verify}"
DOCKERFILE="$ROOT/docker/linux-verify.Dockerfile"
# Default to the full milestone sweep; VERIFY=verify.sh gives the fast loop.
VERIFY="${VERIFY:-verify-full.sh}"
# A named volume, so the Linux build cache survives between runs and stays off the host tree.
# Remove it with: docker volume rm eventlog-linux-build
BUILD_VOLUME="${EVENTLOG_LINUX_BUILD_VOLUME:-eventlog-linux-build}"

step() { printf '\n%%%%%%%%%%  %s\n' "$1"; }

case "$VERIFY" in
  verify.sh | verify-full.sh) ;;
  *)
    echo "VERIFY must be verify.sh or verify-full.sh (got '$VERIFY')" >&2
    exit 64
    ;;
esac

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found. Linux verification needs a container runtime; see ADR-004." >&2
  exit 3
fi
if ! docker info >/dev/null 2>&1; then
  echo "the Docker daemon is not reachable. Start Docker Desktop and retry." >&2
  exit 3
fi

step "Build toolchain image ($IMAGE)"
# Cached after the first run; only a Dockerfile change rebuilds it.
docker build --file "$DOCKERFILE" --tag "$IMAGE" "$ROOT/docker"

step "Run ./scripts/$VERIFY on Linux"
docker run --rm \
  --mount "type=bind,source=$ROOT,target=/src,readonly" \
  --mount "type=volume,source=$BUILD_VOLUME,target=/work" \
  --workdir /src \
  --env "BUILD_ROOT=/work" \
  --env "CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  "$IMAGE" \
  bash -c "uname -srm && cc --version | head -1 && ./scripts/$VERIFY"

step "verify-linux.sh PASS"
