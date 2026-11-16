#!/bin/sh
# Runs inside the lumen-toolchain container.
#   /src   = repo (bind mount)      /kbuild = persistent docker volume (fast disk)
# qemu: Image + initramfs; s8-dreamlte: static userspace + fonts only.
set -eu
PROFILE=${1:-qemu}
[ "$(uname -m)" = aarch64 ] && [ "$(gcc -dumpmachine | cut -d- -f1)" = aarch64 ] ||
  { echo "[build] ARM64 toolchain required" >&2; exit 1; }
KVER=6.12.113
K=/kbuild/linux-$KVER
OUT=/src/build
GEN=/kbuild/gen
JOBS=$(nproc)
case "$PROFILE" in
  qemu)
    OBJ=/kbuild/obj
    BACKEND=drm
    FLAGS="-idirafter $K/usr/include"
    ;;
  s8-dreamlte)
    OUT=$OUT/s8-dreamlte
    OBJ=/kbuild/obj-s8-dreamlte
    BACKEND=fbdev
    FLAGS=-DLUMEN_DREAMLTE
    ;;
  *) echo "[build] unknown profile: $PROFILE" >&2; exit 2 ;;
esac
# Both profiles share the generated assets and BearSSL cache.
exec 9>/kbuild/.build.lock
flock -x 9
mkdir -p "$OUT" "$GEN"

# ---- kernel -------------------------------------------------------------
if [ "$PROFILE" = qemu ]; then
  if [ ! -d "$K" ]; then
    echo "[build] downloading linux-$KVER"
    curl -sSfL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar xJ -C /kbuild
  fi
  FRAG_SUM=$(md5sum /src/kernel/lumen.config | cut -d' ' -f1)
  if [ "$(cat $K/.lumen-frag 2>/dev/null)" != "$FRAG_SUM" ]; then
    echo "[build] configuring kernel"
    make -C "$K" ARCH=arm64 allnoconfig >/dev/null
    (cd "$K" && ARCH=arm64 scripts/kconfig/merge_config.sh -m .config /src/kernel/lumen.config >/dev/null)
    make -C "$K" ARCH=arm64 olddefconfig >/dev/null
    # report fragment options that did not stick
    grep -E '^CONFIG_.*=y' /src/kernel/lumen.config | while read -r l; do
      grep -qx "$l" "$K/.config" || echo "[build] WARNING: $l not set"
    done
    echo "$FRAG_SUM" > "$K/.lumen-frag"
  fi
  make -C "$K" ARCH=arm64 -j"$JOBS" Image >/kbuild/kernel-build.log 2>&1 || { tail -40 /kbuild/kernel-build.log; exit 1; }
  cp "$K/arch/arm64/boot/Image" "$OUT/Image"
  # kernel UAPI headers (drm, input, sound, netlink) straight from this kernel
  [ -f "$K/usr/include/drm/drm.h" ] || make -C "$K" ARCH=arm64 headers >/dev/null
fi

# ---- BearSSL (the one TLS library) --------------------------------------
if [ ! -f /kbuild/bearssl/build/libbearssl.a ]; then
  echo "[build] building BearSSL"
  rm -rf /kbuild/bearssl && cp -r /src/third_party/bearssl /kbuild/bearssl
  make -C /kbuild/bearssl -j"$JOBS" lib tools >/dev/null
fi

# ---- CA bundle -> BearSSL trust anchors ----------------------------------
CA_SUM=$(md5sum /src/third_party/cacert.pem | cut -d' ' -f1)
if [ "$(cat $GEN/.ca 2>/dev/null)" != "$CA_SUM" ]; then
  echo "[build] converting Mozilla CA bundle"
  { echo '#include "bearssl.h"'; /kbuild/bearssl/build/brssl ta -q /src/third_party/cacert.pem;
    echo 'const br_x509_trust_anchor *lumen_tas = TAs; const size_t lumen_tas_num = TAs_NUM;'; } > "$GEN/ta.c"
  echo "$CA_SUM" > "$GEN/.ca"
fi

# ---- fonts: instance the variable font to static weights ------------------
if [ ! -f "$GEN/fonts/B800.ttf" ]; then
  echo "[build] instancing Bricolage Grotesque"
  mkdir -p "$GEN/fonts"
  VF=/src/third_party/fonts/BricolageGrotesque-VF.ttf
  fonttools varLib.instancer -q "$VF" wght=400 opsz=14 wdth=100 -o "$GEN/fonts/B400.ttf"
  fonttools varLib.instancer -q "$VF" wght=600 opsz=14 wdth=100 -o "$GEN/fonts/B600.ttf"
  fonttools varLib.instancer -q "$VF" wght=800 opsz=96 wdth=100 -o "$GEN/fonts/B800.ttf"
fi

# ---- userland: one static binary ----------------------------------------
echo "[build] compiling lumen"
mkdir -p "$OBJ"
CFLAGS="-O2 -g0 -std=gnu11 -Wall -Wno-unused-function -Wno-misleading-indentation $FLAGS -I/src/src -I/src/third_party -I/kbuild/bearssl/inc -I$GEN -pthread"
OBJS=""
PIDS=""
running=0
wait_batch() {
  failed=0
  for pid in $PIDS; do wait "$pid" || failed=1; done
  [ "$failed" = 0 ] || { echo "[build] compile failed" >&2; exit 1; }
  PIDS=""
  running=0
}
for f in $(cd /src/src && find . -name '*.c' | sort); do
  case "$f" in
    ./drm.c|./fbdev.c) [ "$f" = "./$BACKEND.c" ] || continue ;;
  esac
  o=$OBJ/$(echo "$f" | sed 's|^\./||; s|/|_|g; s|\.c$|.o|')
  if [ ! -f "$o" ] || [ "/src/src/$f" -nt "$o" ] || [ -n "$(find /src/src -name '*.h' -newer "$o" | head -1)" ]; then
    echo "  cc $f"
    (gcc $CFLAGS -c "/src/src/$f" -o "$o.tmp" && mv "$o.tmp" "$o") &
    PIDS="$PIDS $!"
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then wait_batch; fi
  fi
  OBJS="$OBJS $o"
done
wait_batch
if [ ! -f "$OBJ/ta.o" ] || [ "$GEN/ta.c" -nt "$OBJ/ta.o" ]; then
  gcc -O2 -I/kbuild/bearssl/inc -c "$GEN/ta.c" -o "$OBJ/ta.o"
fi
gcc -static -pthread -o "$OBJ/lumen" $OBJS "$OBJ/ta.o" /kbuild/bearssl/build/libbearssl.a -lm
strip "$OBJ/lumen"

if [ "$PROFILE" = s8-dreamlte ]; then
  mkdir -p "$OUT/fonts"
  cp "$OBJ/lumen" "$OUT/lumen"
  cp "$GEN/fonts/"*.ttf "$OUT/fonts/"
  echo "[build] done: $OUT/lumen and fonts (ARM64 userspace)"
  exit 0
fi

# ---- initramfs ------------------------------------------------------------
[ -x /kbuild/gen_init_cpio ] || gcc -O2 -o /kbuild/gen_init_cpio "$K/usr/gen_init_cpio.c"
cat > /kbuild/initramfs.list <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
dir /fonts 0755 0 0
file /init $OBJ/lumen 0755 0 0
file /fonts/B400.ttf $GEN/fonts/B400.ttf 0644 0 0
file /fonts/B600.ttf $GEN/fonts/B600.ttf 0644 0 0
file /fonts/B800.ttf $GEN/fonts/B800.ttf 0644 0 0
EOF
/kbuild/gen_init_cpio /kbuild/initramfs.list > "$OUT/initramfs.cpio"
echo "[build] done: $(ls -la $OUT/Image $OUT/initramfs.cpio | awk '{print $5, $9}' | tr '\n' ' ')"
