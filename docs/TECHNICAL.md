# Lumen OS 2 — "Slab"

A phone operating system written from scratch for QEMU (`virt`, aarch64, HVF) on an
Apple Silicon Mac. Everything above the Linux kernel is one C program written for
this project: init, display, compositor, UI toolkit, shell, eight apps, a DHCP client,
DNS resolver, HTTP/1.1 client, an HTML/CSS engine and audio/video playback.

The design is "Slab": solid colour slabs that stack and overlap, giant 800-weight
numerals, accordion drawers instead of an icon grid. No gradients, no blur, no shadows.

## Run it

Requirements on the Mac: Docker (colima is fine), and `brew install qemu yt-dlp ffmpeg`.

```bash
./run.sh
```

then connect a VNC viewer to `localhost:5901`. The mouse is your finger.

`run.sh` builds everything inside an arm64 Alpine container the first time (kernel
~3 min, the rest seconds), creates `data.img` (the phone's 8 MB data disk) if it is
missing, starts the YouTube bridge in the background, boots QEMU with a QMP socket at
`build/qmp.sock`, and stops the bridge again when QEMU exits.
Serial output (kernel + OS log, including frame-time and video stats) goes to
`build/serial.log`.

Useful environment variables: `LUMEN_DATA=path.img` (another data disk),
`LUMEN_AUDIO=wav,id=snd0,path=out.wav` (capture audio instead of playing it),
`LUMEN_NO_BRIDGE=1`.

Tests: `test/e2e.py` boots the phone headless and drives the whole flow through QMP
(see `screenshots/e2e/REPORT.txt`); `test/inflate_check.sh` checks the inflate
implementation against Python's zlib; `test/webdump.sh page.html` dumps the browser's
display list for a saved page.

## What is in it

| | |
|---|---|
| **Boot** | No bootloader: QEMU loads the kernel and initramfs directly. Framebuffer console is off, so the first pixels you see are ours. Boot animation: the three lock bands slam in with spring overshoot and "lumen" types in. |
| **Lock** | Hour and minute as giant numerals bleeding off the edges, lime slab with date, live weather (wttr.in), notifications. Push the lime slab up; the bands above follow with lag like stacked paper. Numerals roll like a slot machine when the minute changes. |
| **Home** | Date numeral, search slab, four drawers (Watch, Make, Play, System) stacked from the bottom; one open at a time, tap or drag tabs. |
| **Apps** | Video (YouTube search + playback), Web, Photos, Notes, Clock, Calculator, Game (2048), Settings. |
| **System** | Home gesture from the bottom edge (the app shrinks back into its tile), control center pulled from the top (real Wi-Fi / airplane toggles, dark/light, mute, brightness and volume slabs, Now Playing), touch circle for recordings. |
| **Persistence** | Notes, settings, accent set, bookmarks, browser history, game best score and video watch history survive reboots (key/value store with A/B slots on the raw data disk; no filesystem needed). |

## Written from scratch vs. third-party

Written for this project (`src/`, about 8,000 lines of C):

- PID 1 init, raw DRM/KMS display driver client with page flips, evdev input
- software rasteriser (signed-distance anti-aliased rounded rects, lines, arcs, bilinear blits, rotation), compositor, springs and fling physics
- the Slab shell, UI toolkit, own on-screen keyboard, line icons, all eight apps
- raw-ioctl ALSA PCM output with a mixer thread (no alsa-lib)
- DHCP client (hand-built Ethernet/IPv4/UDP/BOOTP frames on AF_PACKET), rtnetlink configuration, DNS resolver with cache
- HTTP/1.1 client: keep-alive, chunked, redirects, cookies; RFC 1951 inflate + gzip/zlib wrappers
- HTML tokenizer and tree builder, CSS parser + cascade, block/inline/table layout, forms
- MJPEG + PCM player with audio-clock A/V sync; procedural photo generator; key/value store

Third-party code, and nothing else:

| What | Why |
|---|---|
| Linux 6.12 LTS kernel (incl. its TCP/IP stack and UAPI headers) | underneath everything |
| musl libc (Alpine) | C library |
| `stb_truetype.h` | glyph rasterising |
| `stb_image.h` (PNG/JPEG only) | images in Web, thumbnails and every video frame |
| BearSSL 0.6 | TLS for HTTPS |
| Mozilla CA bundle (`cacert.pem`) | certificate validation, compiled into trust anchors |
| Bricolage Grotesque (OFL) | the only font; instanced to static 400/600/800 with fonttools at build time |

There is no JavaScript engine. The browser renders HTML and CSS only.

## The YouTube bridge, honestly

The phone cannot run YouTube's own player, so a small helper runs on the Mac:
`bridge/ytbridge.py` (Python standard library only, about 220 lines). It

1. calls **yt-dlp** to search YouTube (`/search`) and to resolve the real stream URLs,
   and fetches the 320x180 thumbnails (`/thumb`);
2. runs **ffmpeg** to decode the YouTube stream and re-encode it into a deliberately
   simple format: baseline JPEG frames (640x360, 30 fps) and raw PCM (48 kHz stereo);
3. interleaves those into the packet format documented in `docs/STREAM.md` and sends it
   over plain HTTP.

The phone reaches it at `http://10.0.2.2:8765` through QEMU's virtual network, using its
own DHCP/DNS/HTTP code. From there everything happens on the phone: a reader thread
demuxes packets, a decoder thread decodes every JPEG frame with stb_image, PCM goes
to the virtio-sound device through raw ALSA ioctls, and the player shows the frame that
matches the audio clock (late frames are dropped, audio never waits). Seeking reopens
the stream at a new time. If the bridge is not running, the Video app says so and
offers a retry.

One small convenience: the bridge re-ranks YouTube's 20 results by how many of the
query words appear in the title and channel (a stable sort, YouTube's order is kept
among ties). Without it an exact "title + channel" search can land far down the list
for an unsigned-in search.

## Notes

- DuckDuckGo sometimes answers automated-looking searches with its own "select the
  ducks" challenge page (HTTP 202). The browser renders it like any other page; a human
  can solve it there. Searching through DuckDuckGo's own HTML form (open
  `html.duckduckgo.com/html/`) uses POST and is usually accepted.
- The clock comes from the PL031 RTC with `-rtc base=localtime`, so the phone shows Mac
  local time.
- Architecture and milestones: `ARCHITECTURE.md`. Build timeline: `PROGRESS.log`.
