#!/bin/sh
# Lumen OS 2 — build (if needed), create the data disk, start the YouTube bridge, boot QEMU.
set -e
cd "$(dirname "$0")"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
mkdir -p build

# 1. build if needed
if [ ! -f build/Image ] || [ ! -f build/initramfs.cpio ] || \
   [ -n "$(find src kernel docker third_party -type f -newer build/initramfs.cpio | head -1)" ]; then
  ./build.sh
fi

# 2. data disk (raw, holds notes/settings/history; see src/store.c)
[ -f data.img ] || { echo "Creating data.img"; dd if=/dev/zero of=data.img bs=1048576 count=8 2>/dev/null; }

# 3. YouTube bridge (stopped again when QEMU exits)
BRIDGE=""
if [ -z "$LUMEN_NO_BRIDGE" ]; then
  python3 bridge/ytbridge.py > build/bridge.log 2>&1 &
  BRIDGE=$!
fi
cleanup() { [ -n "$BRIDGE" ] && kill "$BRIDGE" 2>/dev/null; rm -f build/qmp.sock; }
trap cleanup EXIT INT TERM

# 4. QEMU (QMP socket at build/qmp.sock for tests)
AUDIO="${LUMEN_AUDIO:-coreaudio,id=snd0}"
rm -f build/qmp.sock
echo "Connect VNC to localhost:5901"
qemu-system-aarch64 -M virt -accel hvf -cpu host -smp 4 -m 2048 \
  -kernel build/Image -initrd build/initramfs.cpio \
  -append "console=ttyAMA0 loglevel=4" \
  -device virtio-gpu-pci,xres=720,yres=1560 \
  -device virtio-tablet-pci -device virtio-keyboard-pci \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -audiodev "$AUDIO" -device virtio-sound-pci,audiodev=snd0 \
  -drive file=data.img,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
  -device virtio-rng-pci \
  -rtc base=localtime \
  -vnc localhost:1 -display none \
  -serial file:build/serial.log \
  -qmp unix:build/qmp.sock,server,nowait
