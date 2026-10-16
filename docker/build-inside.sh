#!/bin/sh
# Runs inside the lumen-toolchain container.
#   /src   = repo (bind mount)      /kbuild = persistent docker volume (fast disk)
# Produces /src/build/Image and /src/build/initramfs.cpio
set -eu
KVER=6.12.113
K=/kbuild/linux-$KVER
OUT=/src/build
GEN=/kbuild/gen
JOBS=$(nproc)
mkdir -p "$OUT" "$GEN"

# ---- kernel -------------------------------------------------------------
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
mkdir -p /kbuild/obj
CFLAGS="-O2 -g0 -std=gnu11 -Wall -Wno-unused-function -Wno-misleading-indentation -idirafter /kbuild/linux-$KVER/usr/include -I/src/src -I/src/third_party -I/kbuild/bearssl/inc -I$GEN -pthread"
OBJS=""
for f in $(cd /src/src && find . -name '*.c' | sort) ; do
  o=/kbuild/obj/$(echo "$f" | sed 's|^\./||; s|/|_|g; s|\.c$|.o|')
  if [ ! -f "$o" ] || [ "/src/src/$f" -nt "$o" ] || [ -n "$(find /src/src -name '*.h' -newer "$o" | head -1)" ]; then
    echo "  cc $f"
    gcc $CFLAGS -c "/src/src/$f" -o "$o" &
  fi
  OBJS="$OBJS $o"
  # cap parallelism
  while [ "$(jobs -p | wc -l)" -ge "$JOBS" ]; do sleep 0.05; done
done
wait
for o in $OBJS; do [ -f "$o" ] || { echo "compile failed"; exit 1; }; done
if [ ! -f /kbuild/obj/ta.o ] || [ "$GEN/ta.c" -nt /kbuild/obj/ta.o ]; then
  gcc -O2 -I/kbuild/bearssl/inc -c "$GEN/ta.c" -o /kbuild/obj/ta.o
fi
gcc -static -pthread -o /kbuild/lumen $OBJS /kbuild/obj/ta.o /kbuild/bearssl/build/libbearssl.a -lm
strip /kbuild/lumen

# ---- initramfs ------------------------------------------------------------
[ -x /kbuild/gen_init_cpio ] || gcc -O2 -o /kbuild/gen_init_cpio "$K/usr/gen_init_cpio.c"
cat > /kbuild/initramfs.list <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
dir /fonts 0755 0 0
file /init /kbuild/lumen 0755 0 0
file /fonts/B400.ttf $GEN/fonts/B400.ttf 0644 0 0
file /fonts/B600.ttf $GEN/fonts/B600.ttf 0644 0 0
file /fonts/B800.ttf $GEN/fonts/B800.ttf 0644 0 0
EOF
/kbuild/gen_init_cpio /kbuild/initramfs.list > "$OUT/initramfs.cpio"
echo "[build] done: $(ls -la $OUT/Image $OUT/initramfs.cpio | awk '{print $5, $9}' | tr '\n' ' ')"
