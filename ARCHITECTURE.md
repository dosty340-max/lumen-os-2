# Lumen OS 2 — Architecture

Lumen OS 2 is a phone OS for QEMU `virt` (aarch64, HVF). Linux is the only thing
underneath. Everything above the kernel — init, display driver client, compositor,
UI toolkit, shell, apps, network stack client side (DHCP/DNS/HTTP), HTML/CSS
engine, audio and video playback — is one statically linked C program written for
this project (`/init`, built from `src/`).

```
+------------------------------------------------------------------+
|  apps: Video  Web  Photos  Notes  Clock  Calculator  Game  Settings |
+------------------------------------------------------------------+
|  shell: boot anim · lock slabs · home drawers · control center    |
|         gestures · app open/close transitions · touch circle      |
+-----------------------+---------------------+--------------------+
|  ui toolkit (ui.c)    |  web engine (web/)  |  media (video.c,   |
|  slabs, tiles, keys,  |  html tokenizer +   |  audio.c) MJPEG +  |
|  springs, icons       |  tree, css, layout  |  PCM, A/V sync     |
+-----------------------+---------------------+--------------------+
|  gfx.c: canvas, AA shapes, blending, images, rotated blit         |
|  font.c: stb_truetype + glyph cache        store.c: data disk     |
+------------------------------------------------------------------+
|  net/: dhcp (AF_PACKET) · rtnetlink · dns · http/1.1 · tls(BearSSL)|
|        inflate · url · async job queue                            |
+------------------------------------------------------------------+
|  drm.c (raw KMS ioctls) · input.c (evdev) · audio (raw ALSA ioctls)|
+------------------------------------------------------------------+
|  Linux 6.12 LTS (minimal config, no fbcon) · musl libc            |
+------------------------------------------------------------------+
```

## Boot flow
1. QEMU loads `build/Image` + `build/initramfs.cpio` directly (`-kernel/-initrd`).
2. Kernel has no framebuffer console and no VT; all kernel text goes to the PL011
   serial port (`build/serial.log`). The screen stays black until we draw.
3. `/init` (PID 1) mounts proc, sys, devtmpfs, tmpfs on /tmp; then forks the UI
   process. PID 1 stays as a reaper and restarts the UI if it ever crashes, and
   performs `reboot(2)` when the UI asks for it.
4. UI process: opens DRM, input, data disk, starts the network thread (DHCP),
   plays the boot animation, then shows the lock screen.

## Rendering
- `drm.c` opens `/dev/dri/card0`, finds the connector/CRTC, creates two dumb
  buffers (720x1560 XRGB8888), mmaps them, and page-flips between them with
  `DRM_MODE_PAGE_FLIP_EVENT` (waits for the flip event = vsync-ish). Falls back to
  SETCRTC if page flip is refused.
- The whole frame is redrawn every frame in software (immediate mode). The scene
  is cheap: solid slabs, AA rounded rects (per-pixel coverage on corners only),
  cached glyph bitmaps and a few images. Target 60 fps; frame times are logged.
- Coordinates: design is specified in logical points for a 390x844 phone; the
  physical screen is 720x1560, so `S = 720/390 = 1.846` and everything is drawn in
  physical pixels via `P(x)`.
- Layers: apps render into an offscreen canvas; the compositor blits it with a
  rect transform (open/close/home-gesture shrink) or a rotated transform
  (fullscreen landscape video).
- Text: Bricolage Grotesque instanced at build time into 400/600/800 static TTFs
  (fonttools in Docker). `font.c` rasterises glyphs with stb_truetype into a hash
  glyph cache (font, pixel size, codepoint) and blends them with alpha.
- Touch circle: a translucent disc drawn last, wherever the pointer is pressed.

## Input
- `virtio-tablet-pci` → evdev ABS_X/ABS_Y (0..32767) + BTN_LEFT = one finger.
- `virtio-keyboard-pci` → evdev keys, mapped to UTF-8 with shift.
- `input.c` polls all `/dev/input/event*` non-blocking each frame and produces
  down/move/up and key events. A gesture layer tracks velocity (last 80 ms) for
  flicks and recognises tap vs drag (8 px slop) and edge swipes (bottom 24 pt =
  home, top edge = control center).

## Animation
Critically/under-damped springs (`spring_step`) drive every motion: slab drags,
drawer accordion, app expand/collapse, control center, scroll flings (exponential
friction with spring-back at the ends), numeral slot-machine roll.

## Persistence
`run.sh` creates `data.img` (8 MB raw) on first run. There is no filesystem:
`store.c` keeps a tiny key/value store (named blobs) serialized into one of two
slots (A/B with sequence number + checksum) on `/dev/vda`, so a crash during a
write never loses the previous state. Stored: notes, settings (accent set, dark
mode, brightness, volume, text size), bookmarks, browser history, game best
score, video watch history.

## Networking (`src/net/`)
- **netif**: rtnetlink (`RTM_NEWLINK` up/down, `RTM_NEWADDR`, `RTM_NEWROUTE`),
  written by hand over an `AF_NETLINK` socket.
- **dhcp**: raw `AF_PACKET` socket; builds Ethernet/IPv4/UDP frames itself
  (checksums included), DISCOVER → OFFER → REQUEST → ACK, then configures
  address/route via netif. States: no link / connecting / connected.
- **dns**: UDP to the DHCP-provided server (10.0.2.3), A records, follows CNAME
  answers in the same packet, cache with TTL, 2 s timeout, 3 tries.
- **http**: HTTP/1.1 client with keep-alive connection pool, chunked transfer,
  Content-Length, redirects (max 5), in-memory cookie jar,
  `Accept-Encoding: identity`, own inflate (RFC1951 + gzip wrapper) if a server
  gzips anyway, streaming reads for the video stream.
- **tls**: BearSSL client with the Mozilla CA bundle compiled into trust anchors
  at build time (`brssl ta`), SNI, real clock from the RTC.
- **jobs**: requests run on worker threads; results are posted to a queue drained
  by the UI thread, so the UI never blocks on the network.

## Browser pipeline (`src/web/`)
URL / search words → http fetch (worker) → charset: UTF-8 (latin-1 fallback) →
**HTML tokenizer** (states for tags, attributes, comments, raw text for
script/style, entities) → **tree builder** (implied html/head/body, auto-close
p/li/dt/dd/tr/td/option, void elements, misnested end tags tolerated) →
**CSS**: UA stylesheet + `<style>` + `<link rel=stylesheet>` (fetched) + `style=""`;
selectors tag/.class/#id/descendant/compound, specificity cascade, `@media
(max-width)` evaluated at 390 px → **style computation** (display, colors, font
size/weight, text-align, margin/padding/border, width/max-width, white-space pre)
→ **layout**: block formatting + inline formatting with word wrap, list markers,
tables (column widths from content, rows/cells), images (size from attributes,
decoded on a worker with stb_image, relayout when they arrive) → **display list**
(rects, text runs, images, link boxes) → painted with the scroll offset. Links are
hit-tested on the display list. Forms: inputs become editable boxes; submit builds
a GET query or POST urlencoded body. No JavaScript (stated in About).

## Video pipeline
```
Mac:  yt-dlp (search/urls) → ffmpeg (decode, scale 640x360, 30 fps,
      JPEG q, PCM s16le 48k stereo) → ytbridge.py muxes packets → HTTP
Phone: http stream reader thread → packet demux
          ├─ 'V' JPEG packets → compressed queue → decoder thread (stb_image)
          │                                      → decoded ring (8 frames)
          └─ 'A' PCM packets  → PCM ring (10 s) → audio thread → ALSA writei
       UI thread: clock = audio frames played (written - delay) → show newest
       frame with pts <= clock, drop late frames. Audio is master; video waits.
```
Container format is documented in `docs/STREAM.md`. Seeking closes the stream
and reopens it with `t=`. Buffering spinner while the PCM ring is below 0.3 s.

## Audio
`audio.c` opens `/dev/snd/pcmC0D0p` and talks raw ALSA ioctls: HW_PARAMS
(interleaved, S16_LE, 2 ch, 48 kHz, period 1024, buffer 8192), SW_PARAMS,
PREPARE, WRITEI_FRAMES, DELAY; recovers from underruns with PREPARE. One mixer
thread mixes the video PCM and UI sounds (timer alarm, test tone, clicks) and
applies volume/mute.

## Bridge (`bridge/ytbridge.py`, runs on the Mac)
Python stdlib HTTP server on 127.0.0.1:8765 (QEMU slirp exposes it to the guest
as 10.0.2.2:8765). `/search` → `yt-dlp ytsearch20: --flat-playlist -J`;
`/thumb` → i.ytimg.com mqdefault (320x180) re-encoded by ffmpeg to baseline JPEG;
`/stream` → `yt-dlp -g` for stream URLs, then one ffmpeg process with two pipe
outputs (MJPEG to fd 3, PCM to fd 4), muxed into our packet format.

## Build
`run.sh` → `docker build` (Alpine aarch64 toolchain image) → container runs
`docker/build-inside.sh`: kernel (cached in a docker volume, built from
`kernel/lumen.config` fragment on top of allnoconfig), BearSSL static lib, CA
trust anchors, font instancing, then compiles `src/` with musl `-static -O2`
and packs the initramfs with the kernel's `gen_init_cpio`.

## Milestones
1. M1 boot to a colored screen
2. M2 compositor + text + input + touch circle
3. M3 boot animation + lock screen + home with drawers + gestures
4. M4 status bar + control center + audio output (test tone)
5. M5 offline apps (Photos, Notes, Clock, Calculator, Game, Settings)
6. M6 DHCP + DNS + HTTP + HTTPS (fetch https://example.com, log it)
7. M7 Web browser
8. M8 bridge + Video app with A/V sync
9. M9 polish, performance, final e2e tests, docs
