#!/bin/sh
# Compresses sample data with python's gzip/zlib (all block types) and checks our inflate restores it.
cd "$(dirname "$0")/.."
python3 -c "
import gzip,zlib,random,sys
random.seed(1)
txt=open('ARCHITECTURE.md','rb').read()*3
rnd=bytes(random.getrandbits(8) for _ in range(70000))
open('build/infl_a.gz','wb').write(gzip.compress(txt,9)); open('build/infl_a.raw','wb').write(txt)
open('build/infl_b.gz','wb').write(gzip.compress(rnd,1)); open('build/infl_b.raw','wb').write(rnd)
open('build/infl_c.gz','wb').write(zlib.compress(txt,0)); open('build/infl_c.raw','wb').write(txt)"
docker run --rm -v "$PWD":/src lumen-toolchain sh -c '
  gcc -O2 -I/src/src -include stdint.h -o /tmp/ic /src/test/inflate_check.c /src/src/net/inflate.c -DINFLATE_STANDALONE 2>&1 | grep -v warning
  for t in a b c; do /tmp/ic < /src/build/infl_$t.gz | cmp - /src/build/infl_$t.raw && echo "inflate $t OK"; done'
