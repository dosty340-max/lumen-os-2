Build a complete custom phone operating system from scratch called **Lumen OS 2**. It must boot in QEMU on my M4 MacBook Air (Apple Silicon, arm64, macOS). It's for a YouTube video, so it has to look and feel like a real, polished phone. It also needs WORKING INTERNET: a web browser and a video app where I can search YouTube and play a video.

Work in ~/Documents/PhoneOS2.

## Hard rules
- You write all the OS code yourself. The Linux kernel underneath is allowed, including its TCP/IP network stack. Everything above the kernel is yours: init (PID 1), graphics, compositor, UI toolkit, shell, apps, DHCP client, DNS resolver, HTTP client, HTML/CSS parser and renderer, audio output, and video playback.
- Do NOT use Android, AOSP, postmarketOS, Plasma Mobile, Phosh, Ubuntu Touch, Sailfish, or any existing phone OS or shell.
- Do NOT use GUI frameworks or toolkits: no Qt, GTK, SDL, LVGL, Flutter, Electron, Wayland/X11, or Mesa. No browser engines (WebKit, Blink, Gecko, Servo, litehtml, NetSurf, etc.). No curl/libcurl, no alsa-lib, no busybox/udhcpc, no ffmpeg/libav on the phone side.
- Allowed third-party code, and NOTHING else:
  - the Linux kernel
  - a libc (musl)
  - single-header stb_truetype and stb_image (stb_image may decode PNG/JPEG, including video frames)
  - exactly ONE TLS library: BearSSL (preferred) or mbedTLS
  - the Mozilla CA bundle as data
  - one open-licensed font: Bricolage Grotesque (OFL)
- Generate wallpapers, icons, and demo photos procedurally in code or as your own SVG/PNG. No downloaded stock assets.
- The only code that runs on the Mac (not in the phone) is the YouTube bridge helper described below.

## Platform & build
- Target: QEMU `virt`, aarch64, `-accel hvf -cpu host`.
- Build inside an arm64 Linux Docker container. The only build dependencies on the Mac are Docker plus `brew install qemu yt-dlp ffmpeg`.
- Boot directly with `-kernel` + `-initrd` (no bootloader). Write a minimal kernel config containing:
  - virtio-gpu (DRM/KMS), virtio-input, virtio-blk, pl031 RTC
  - virtio-net + IPv4 + AF_PACKET (for your DHCP client) + UDP/TCP
  - virtio-sound (CONFIG_SND_VIRTIO) + ALSA core (you talk to /dev/snd/pcm* with raw ioctls)
  - Framebuffer console OFF, so no kernel text or penguin is ever visible. Boot output goes to the serial log only.
- Display: portrait 720x1560, `-device virtio-gpu-pci,xres=720,yres=1560`.
- Input: `virtio-tablet-pci` (absolute coordinates, so the mouse is a finger) + `virtio-keyboard-pci`, read via evdev.
- Network: `-netdev user,id=n0 -device virtio-net-pci,netdev=n0`. The guest gets its address from QEMU's built-in DHCP using YOUR DHCP client. DNS goes to 10.0.2.3 using YOUR resolver. The Mac host is reachable at 10.0.2.2.
- Audio: `-audiodev coreaudio,id=snd0 -device virtio-sound-pci,audiodev=snd0`, so sound comes out of the Mac speakers.
- VNC: `-vnc localhost:1` (port 5901), with the guest screen shown 1:1.
- Persistence: a small data disk image that run.sh creates on first run. It stores notes, settings, wallpaper, bookmarks, browser history, the game high score, and video watch history, all of which survive a reboot.
- Clock: `-rtc base=localtime`.
- One `run.sh` in the repo root. It:
  1. builds if needed
  2. creates the data disk if missing
  3. starts the YouTube bridge in the background (and stops it on exit)
  4. starts QEMU with a QMP socket for testing
  5. prints "Connect VNC to localhost:5901"

## Networking stack (all yours, in userspace)
- DHCP client over AF_PACKET (DISCOVER/OFFER/REQUEST/ACK). It configures the interface, address, and route through netlink (rtnetlink) calls you write yourself.
- DNS resolver: UDP, A records, with a cache and a timeout/retry.
- HTTP/1.1 client: keep-alive, chunked transfer, redirects (max 5), Content-Length, cookies kept in memory.
  - Send `Accept-Encoding: identity`. If a server gzips anyway, implement inflate yourself (no zlib).
- HTTPS via the one TLS library, with certificate validation against the bundled CA list and a real clock.
- Expose all of this as a small internal library (`net/`) that both the Web and Video apps use.
- The status bar network icon reflects real state: no link / connecting / connected.
- The control center Wi-Fi toggle really brings the interface down and up and re-runs DHCP.
- Settings > Network shows the IP, gateway, DNS, and a "Test connection" button.

## Web browser app (your own renderer)
- Address bar: typing a URL loads it. Typing words searches via `https://html.duckduckgo.com/html/?q=...` (HTML-only, no JS needed).
- HTML: your own tokenizer + tree builder, tolerant of messy real-world HTML (implied tags, unclosed p/li, entities, UTF-8).
- CSS: a practical subset from `<style>`, `<link rel=stylesheet>`, and style="" attributes. It must cover:
  - selectors: tag, class, id, descendant
  - display block/inline/none/list-item
  - color, background-color, font-size, font-weight, text-align, margin, padding, border, width/max-width
  - `@media (max-width)` evaluated at phone width
  - Ignore what you don't support, gracefully.
- Layout: block + inline formatting with word wrap, lists, headings, basic tables (rows/cells), images (PNG/JPEG via stb_image, decoded progressively off the UI thread), link hit-testing.
- No JavaScript, and say so in About.
- Features:
  - smooth scrolling with fling physics
  - back/forward
  - loading progress bar in the Slab accent
  - tap links
  - text input fields + form submit (GET and POST urlencoded), so search boxes on simple sites work
  - 3+ tabs
  - bookmarks
  - pinch-free "text size" setting
- Must render readably:
  - example.com
  - html.duckduckgo.com search results
  - en.m.wikipedia.org articles
  - news.ycombinator.com
  - lite.cnn.com
  - text.npr.org

## YouTube: bridge + Video app
Honest architecture (I will say this in the video): a tiny helper on the Mac uses yt-dlp to search YouTube and fetch streams, then transcodes with ffmpeg to a simple format. The phone talks to it over the virtual network, decodes every frame and audio sample itself, and plays it.

- `bridge/ytbridge.py`:
  - Python 3 stdlib only, plus calling the `yt-dlp` and `ffmpeg` CLIs.
  - Listens on 127.0.0.1:8765. The guest reaches it at http://10.0.2.2:8765.
  - Keep it small and readable (aim for under 250 lines) because it will be shown on screen.
  - Endpoints:
    - `GET /search?q=...` → JSON list (up to 20): id, title, channel, duration, view count, upload date
    - `GET /thumb?id=...` → 320x180 JPEG
    - `GET /stream?id=...&t=SECONDS&h=360` → one interleaved stream in a simple container YOU design and document in docs/STREAM.md:
      - video as baseline JPEG frames (MJPEG, 640x360, 30 fps)
      - audio as PCM s16le 48 kHz stereo
      - each packet: type, pts in ms, length, payload
  - Seeking reopens the stream at t=.
- Video app on the phone:
  - Search screen: search field, a results list with thumbnails, title, channel, duration, and views. Infinite scroll is not needed.
  - Player:
    - 16:9 video at the top, with title/channel under it
    - play/pause, a scrub bar with finger-follow seeking, elapsed/total time, a buffering spinner
    - a fullscreen button that rotates to landscape inside your compositor with an animation
  - Decoding:
    - decode JPEG frames on a worker thread into a ring buffer
    - write PCM to the virtio-sound device
    - A/V sync with the audio clock as master: drop late frames, never let audio stutter
  - "Recently watched" on the Video home, saved to disk.
  - Must play smoothly at 30 fps with audio in sync for at least 5 minutes.
- If the bridge isn't running, the Video app shows a friendly error with a retry, not a crash.

## Design: "Slab" (follow this exactly — it's the reason for this video)
The comment that started this said the first version looked like an iOS clone. This one must look like NOTHING else: no icon grid, no dock, no iOS/Android/Ubuntu Touch/Sailfish patterns.

**Tokens**
- Colors:
  - orange #FF6A2B
  - lilac #C8B8FF
  - lime #DFFF5E
  - sky #7FD4FF
  - paper #F3F0E8
  - ink #141414
  - muted-on-ink #C9C5BC
- Solid color fields only: no gradients, no glass blur, no drop shadows. Depth comes from overlapping slabs with 30px top corner radii.
- Font: Bricolage Grotesque only. It's a variable font, so at build time instance it to static TTFs (weights 400, 600, 800) with fonttools in Docker, since stb_truetype can't do variations.
  - Huge numerals: 800 weight, letter-spacing about -0.06em.
  - Headings: 800. Labels: 600.
- Icons: your own 2px-stroke line icons in ink or paper. Never emoji.
- Text on colored slabs is always ink. Text on ink is paper or muted.

**Boot animation (2-4 s)**
On black, the three lock bands (orange, lilac, lime) slam in from alternating sides with spring overshoot. The word "lumen" types in at 800 weight, then everything settles into the lock screen.

**Lock screen**
The screen is split into three full-width horizontal bands:
1. Top, orange, about 310px of 844 logical: a small status line ("lumen" left, network + battery right), then the HOUR as a giant numeral (about 250pt logical), left-aligned and bleeding slightly off the left edge.
2. Middle, lilac, about 310px: the MINUTES as a giant numeral, right-aligned and bleeding off the right edge.
3. Bottom, lime, with 30px top radii and a small ink grab bar. It holds:
   - "Fri 16 Oct" (800) on the left, the weather on the right
   - up to 2 notifications as "**Sender**  text" rows
   - "↑ PUSH THIS SLAB UP TO OPEN"

Interactions:
- Dragging the lime slab up follows the finger. The lilac and orange bands get pushed up with slight lag, like stacked paper.
- Past the threshold, everything springs off-screen to reveal home. A short drag springs back.
- The numerals tick with a vertical slot-machine roll when the minute changes.

**Home screen (ink background)**
- Status line at the top: time left, network + battery right, in paper text.
- Header: today's date "Fri 16" as a big 800 numeral on the left, small muted info on the right (weather, timers running).
- A full-width paper "Search the web" slab (58px tall, 22px radius). Tapping it opens Web with the keyboard up.
- DRAWERS: colored slabs stacked from the bottom, each overlapping the one above with 30px top radii. Collapsed drawers show only a ~62px tab: name (800, 24pt) on the left, live info + small icons on the right.
  - Watch (orange): Video [large tile, black, spans 2 columns, shows "Continue · 12 min left" when there's history], Web, Photos
  - Make (lime): Notes, Clock, Calculator (live info: running timer)
  - Play (sky): Game (live info: best score)
  - System (paper): Settings
  - Exactly one drawer is open at a time (accordion). Tapping a collapsed tab springs it open while the others compress. Watch is open by default.
  - Inside an open drawer, apps are tiles in a 3-column grid: outlined in 2px ink, 22px radius, icon top-left, name bottom-left.
- Opening an app: the tile expands to fill the screen in the drawer's color, then the app content fades in. Closing reverses it back into the tile.

**System chrome**
- Home gesture: swipe up from the bottom 24px edge. A short ink/paper bar marks it inside apps. The app shrinks back into its tile.
- Control center: pull down from the top edge. Every color is a solid slab stacking down from the top, mirroring the drawers. It contains:
  - toggles: Wi-Fi (real), Airplane (real: network off), Dark/Light, Mute
  - brightness and volume sliders as thick slabs you drag across
  - a Now Playing slab when video is playing
- Every app's own UI uses the same language: solid slabs, big 800 headings, outlined tiles, no shadows.
- Draw a subtle translucent touch circle wherever the finger (mouse) is pressed, so touches show in the recording.

## Apps (all must actually work)
1. **Video** — as above (search YouTube, play with sound, seek, fullscreen).
2. **Web** — as above.
3. **Photos** — 12+ generated photos in a grid, tap for fullscreen, swipe between them.
4. **Notes** — list + editor, create/edit/delete, saved to disk. Physical keyboard plus your own on-screen keyboard in Slab style (big flat keys, lime press flash).
5. **Clock** — live clock, stopwatch, and a timer that counts down and alerts with a sound.
6. **Calculator** — full, correct, with press animations.
7. **Game** — one small, smooth game (2048-style is fine) with touch controls and a high score saved to disk.
8. **Settings**:
   - wallpaper replaced by "accent set" picker: at least 4 band color combos for the lock screen, live preview
   - dark/light
   - brightness, volume
   - Network page
   - text size
   - About (OS name, version, "built by Claude", list of the only third-party code used)

Do not add apps that don't work (no fake Phone/Camera/Messages).

## Rendering / feel
- Your own compositor drawing to DRM/KMS with double buffering and vsync-ish page flips, targeting a steady 60 fps.
- Optimized software rendering is fine: anti-aliased shapes, rounded rects, alpha blending, smooth anti-aliased text with a glyph cache.
- Real physics everywhere: finger-follow dragging, velocity flicks, spring animations. Nothing jumps or snaps without animation.
- Network, image decoding, and video decoding must never block the UI thread.

## Process
1. Before writing code, write ARCHITECTURE.md (components, boot flow, rendering, input, networking, browser pipeline, video pipeline, bridge) and a milestone plan.
2. Keep a PROGRESS.log in the repo root. Append one line per meaningful step in the format `HH:MM  what was done` (local time from `date +%H:%M`). I'll use it for the video timeline.
3. Build in milestones. Each one must boot and be verified before you move on. Commit to git after each.
   - M1 boot to a colored screen
   - M2 compositor + text + input + touch circle
   - M3 boot animation + lock screen + home with drawers + gestures
   - M4 status bar + control center + audio output (test tone)
   - M5 offline apps
   - M6 DHCP + DNS + HTTP + HTTPS (fetch example.com over HTTPS, log it)
   - M7 Web browser
   - M8 bridge + Video app with A/V sync
   - M9 polish, performance, final tests

## Testing (mandatory, do it yourself)
- Run QEMU headless with QMP. Use `screendump` for screenshots and `input-send-event` (absolute coordinates) to tap, swipe, and type.
- Write `test/e2e.sh` (or Python). It must:
  1. boot and screenshot the boot animation
  2. drag the lime slab up to unlock
  3. open and close each drawer
  4. use every app: calculate 12×3=36, save a note, change the accent set, toggle dark mode, run the timer, play the game
  5. open Web, load https://example.com and an en.m.wikipedia.org article, scroll, tap a link, go back
  6. search "lumen os" from the home search slab
  7. open Video and search for the query in the env var `TEST_QUERY` (default: "What If the Internet Went Down for 24 Hours The Render Guy")
  8. open the first result, play 20 s, seek to 1:00, go fullscreen, check consecutive screenshots differ (video is moving), and check the audio device got samples
  9. pull down control center, toggle Wi-Fi off and on and confirm reconnection, swipe home
  10. reboot and confirm the note, accent set, and watch history persisted
- LOOK at every screenshot yourself and fix anything ugly, misaligned, clipped, or broken. The Slab look must match the spec above. Visual quality counts as much as function.
- Log frame times. UI animations should stay near 60 fps, and video should play at 30 fps without drops for 5 minutes.

## Definition of done
- `./run.sh` on a clean M4 Mac (with only Docker, QEMU, yt-dlp, and ffmpeg installed) builds and boots, and the whole flow works over VNC on localhost:5901 using only the mouse.
- I can type my own video's title in the Video app, find it, and watch it with sound.
- All e2e tests pass, and screenshots of every screen (including a playing video and a rendered Wikipedia page) are saved in `screenshots/`.
- README.md explains how to run it, lists exactly what was written from scratch versus the allowed third-party code, and explains honestly what the Mac bridge does.
- Do not say you're done until all of the above is verified with your own screenshots.