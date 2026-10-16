#!/bin/sh
# dev helper: (re)build and boot headless in the background; audio goes to build/audio.wav
cd "$(dirname "$0")/.."
python3 test/q.py quit 2>/dev/null; sleep 0.5
pkill -f qemu-system-aarch64 2>/dev/null; sleep 0.3
./build.sh > build/build.log 2>&1 || { grep -E "error|warning" build/build.log | head -30; exit 1; }
grep -E "warning" build/build.log | head -20
LUMEN_NO_BRIDGE=${LUMEN_NO_BRIDGE:-1} LUMEN_AUDIO="wav,id=snd0,path=build/audio.wav" nohup ./run.sh > build/run.out 2>&1 &
sleep ${1:-3}
