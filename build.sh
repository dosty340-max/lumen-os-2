#!/bin/sh
# Build in native ARM64 Alpine (Docker may emulate ARM64 on other hosts).
set -eu
cd "$(dirname "$0")"
profile=${1:-qemu}
case "$profile" in
  qemu|s8-dreamlte) ;;
  *) echo "usage: $0 [qemu|s8-dreamlte]" >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || { echo "usage: $0 [qemu|s8-dreamlte]" >&2; exit 2; }
if [ "$(docker image inspect --format '{{.Architecture}}' lumen-toolchain 2>/dev/null)" != arm64 ]; then
  docker build --platform linux/arm64 -t lumen-toolchain docker
fi
docker run --rm --platform linux/arm64 -v "$PWD":/src -v lumen-kbuild:/kbuild \
  lumen-toolchain sh /src/docker/build-inside.sh "$profile"
