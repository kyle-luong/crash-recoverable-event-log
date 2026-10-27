# Toolchain for ./scripts/verify-linux.sh. Linux is the supported platform (ADR-004), so this
# image is where verification evidence is produced.
#
# Pinned to a release tag rather than :latest so the toolchain does not move under the project
# between runs. No project dependencies are installed here: the library itself takes none
# (docs/PRD.md section 3), and these packages are the compiler, build driver and formatter only.
FROM debian:bookworm-20250929-slim

RUN apt-get update \
  && apt-get install --no-install-recommends -y \
       build-essential \
       clang \
       clang-format \
       cmake \
       ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# Build output goes here, never into the read-only source mount.
RUN mkdir -p /work
WORKDIR /src
