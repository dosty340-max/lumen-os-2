#!/bin/sh
# usage: test/webdump.sh page.html [base-url] [extra.css ...]   (paths relative to repo)
cd "$(dirname "$0")/.."
docker run --rm -e MAX -v "$PWD":/src -v lumen-kbuild:/kbuild lumen-toolchain sh -c '
  mkdir -p /fonts && cp /kbuild/gen/fonts/*.ttf /fonts/
  OBJS=$(ls /kbuild/obj/*.o | grep -v -e "/main.o" -e "/ta.o")
  gcc -O1 -std=gnu11 -idirafter /kbuild/linux-6.12.113/usr/include -I/src/src -I/src/third_party -I/kbuild/bearssl/inc -pthread -o /tmp/wd /src/test/webdump.c $OBJS /kbuild/obj/ta.o /kbuild/bearssl/build/libbearssl.a -lm 2>&1 | grep -v warning
  cd /src && /tmp/wd "$@"' sh "$@"
