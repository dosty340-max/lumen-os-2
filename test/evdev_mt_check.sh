#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
docker image inspect lumen-toolchain >/dev/null 2>&1 || docker build -t lumen-toolchain docker
docker run --rm -v "$PWD":/src:ro lumen-toolchain sh -ec '
  gcc -std=gnu11 -Wall -Wextra -Werror -I/src/src -c /src/src/evdev_mt.c -o /tmp/evdev_mt.o
  gcc -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections -I/src/src \
    -o /tmp/check /src/test/evdev_mt_check.c /tmp/evdev_mt.o /src/src/ui.c -lm
  /tmp/check
'
