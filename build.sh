#!/bin/sh
# Builds kernel + initramfs inside the arm64 Alpine container.
set -e
cd "$(dirname "$0")"
docker image inspect lumen-toolchain >/dev/null 2>&1 || docker build -t lumen-toolchain docker
docker run --rm -v "$PWD":/src -v lumen-kbuild:/kbuild lumen-toolchain sh /src/docker/build-inside.sh
