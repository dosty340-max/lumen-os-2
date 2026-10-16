<p align="center">
  <img src="docs/media/banner.jpg" alt="Lumen OS 2: a phone OS written from scratch by Claude" width="100%">
</p>

<p align="center">
  <a href="#run-it"><img alt="Runs in QEMU" src="https://img.shields.io/badge/runs%20in-QEMU%20aarch64-FF6A2B?style=for-the-badge"></a>
  <img alt="Language C" src="https://img.shields.io/badge/written%20in-C-C8B8FF?style=for-the-badge&labelColor=141414">
  <img alt="Lines of code" src="https://img.shields.io/badge/own%20code-8%2C227%20lines-DFFF5E?style=for-the-badge&labelColor=141414">
  <img alt="Tests" src="https://img.shields.io/badge/e2e-32%2F32%20passing-7FD4FF?style=for-the-badge&labelColor=141414">
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/badge/license-MIT-F3F0E8?style=for-the-badge&labelColor=141414"></a>
</p>

<h3 align="center">No Android. No iOS. No UI toolkit. No browser engine.<br>Just a Linux kernel, and everything above it written by Claude.</h3>

<p align="center">
  <a href="#-what-it-can-do">Features</a> ·
  <a href="#-screenshots">Screenshots</a> ·
  <a href="#-how-it-works">How it works</a> ·
  <a href="#run-it">Run it</a> ·
  <a href="#-the-story">The story</a> ·
  <a href="docs/TECHNICAL.md">Technical docs</a>
</p>

---

<table>
<tr>
<td width="50%" align="center">
  <img src="docs/media/demo_slab.gif" width="300" alt="Boot, lock screen, unlock and the drawer home screen"><br>
  <sub><b>Boot → lock → push the slab → drawers</b></sub>
</td>
<td width="50%" align="center">
  <img src="docs/media/demo_internet.gif" width="300" alt="Wikipedia in the phone's own browser, then YouTube playing and rotating"><br>
  <sub><b>Its own browser → YouTube → fullscreen rotation</b></sub>
</td>
</tr>
</table>

## ✦ What it can do

| | |
|---|---|
| 🟧 **Slab design** | Solid colour slabs that stack and overlap, giant numerals, accordion drawers instead of an icon grid. No gradients, no glass, no shadows. Designed to look like nothing else. |
| 🔒 **Lock screen** | The hour and the minutes bleed off the edges, live weather from the internet, notifications. Push the lime slab up and the bands follow like stacked paper. |
| 🗂 **Home** | Four drawers (Watch, Make, Play, System). One opens at a time, with springs and fling physics everywhere. |
| 🌐 **Real internet** | Its own DHCP client (built from raw Ethernet frames), DNS resolver, HTTP/1.1 client, inflate, and HTTPS through BearSSL. |
| 🧭 **Its own web browser** | HTML tokenizer and tree builder, CSS cascade, block/inline/table layout, forms, tabs, bookmarks. Wikipedia, Hacker News and CNN Lite render on the phone. |
| ▶️ **YouTube** | Search, play with sound, seek, fullscreen with an animated rotation. Every frame and audio sample is decoded on the phone (30 fps, 0 dropped). |
| 📱 **Apps** | Video, Web, Photos, Notes (own keyboard), Clock (stopwatch, timer), Calculator, 2048, Settings. Everything persists across reboots. |
| 🎛 **System** | Home gesture, control center with real Wi-Fi and airplane toggles, brightness and volume slabs, Now Playing, dark and light themes, accent colour sets. |

## ✦ Screenshots

<p align="center">
  <img src="screenshots/e2e/03_lock_screen.png" width="190" alt="Lock screen">
  <img src="screenshots/e2e/05_home.png" width="190" alt="Home screen">
  <img src="screenshots/e2e/06_drawer_make.png" width="190" alt="Make drawer">
  <img src="screenshots/e2e/43_control_center_now_playing.png" width="190" alt="Control center">
</p>
<p align="center">
  <img src="screenshots/e2e/27_web_wikipedia.png" width="190" alt="Wikipedia in the browser">
  <img src="screenshots/e2e/37_video_results.png" width="190" alt="YouTube search">
  <img src="screenshots/e2e/38_video_playing_20s.png" width="190" alt="Video playing">
  <img src="screenshots/e2e/10_calculator_12x3.png" width="190" alt="Calculator">
</p>
<p align="center">
  <img src="screenshots/e2e/11_notes_editor_keyboard.png" width="190" alt="Notes with its own keyboard">
  <img src="screenshots/e2e/22_photos_grid.png" width="190" alt="Photos">
  <img src="screenshots/e2e/21_game_played.png" width="190" alt="2048">
  <img src="screenshots/e2e/14_settings_accent_sunset.png" width="190" alt="Settings: accent sets">
</p>

<p align="center"><sub>Every screenshot was taken by the end-to-end test itself (QMP <code>screendump</code>). More in <a href="screenshots/">screenshots/</a>.</sub></p>

## ✦ How it works

```mermaid
flowchart LR
    APPS["Slab shell<br/>UI toolkit<br/>8 apps"] --> GFX["Rasteriser<br/>compositor<br/>60 fps"]
    APPS --> WEB["HTML + CSS<br/>engine"]
    APPS --> VID["MJPEG + PCM<br/>player"]
    WEB --> NET["DHCP · DNS<br/>HTTP · HTTPS"]
    VID --> NET
    GFX --> K["Linux 6.12<br/>kernel"]
    NET --> K
    K --> Q["QEMU virt<br/>aarch64 · HVF"]
    NET -. "10.0.2.2" .-> B["Mac helper<br/>yt-dlp + ffmpeg"]
    style APPS fill:#FF6A2B,color:#141414,stroke:#141414
    style GFX fill:#C8B8FF,color:#141414,stroke:#141414
    style WEB fill:#DFFF5E,color:#141414,stroke:#141414
    style VID fill:#7FD4FF,color:#141414,stroke:#141414
    style NET fill:#F3F0E8,color:#141414,stroke:#141414
    style K fill:#141414,color:#F3F0E8,stroke:#141414
    style Q fill:#141414,color:#F3F0E8,stroke:#141414
    style B fill:#ffffff,color:#141414,stroke:#999999,stroke-dasharray: 4 4
```

Everything left of the kernel is one C program running as PID 1.

- **Boot:** no bootloader. QEMU loads the kernel and initramfs directly, the framebuffer console is off, so the first pixels you see are drawn by Lumen.
- **Rendering:** a hand-written software rasteriser (signed-distance anti-aliased shapes, glyph cache) feeding double-buffered DRM page flips at a steady 60 fps.
- **Networking:** the kernel only drives the virtual network card and TCP/IP. DHCP, DNS, HTTP, gzip and the browser are Lumen's own code.

### The YouTube bridge, honestly

The phone can't run YouTube's own player, so a small helper runs on the Mac: [`bridge/ytbridge.py`](bridge/ytbridge.py) (Python standard library, about 220 lines). It uses **yt-dlp** to search and resolve streams and **ffmpeg** to re-encode them into a deliberately simple format: baseline JPEG frames plus raw PCM, interleaved as described in [`docs/STREAM.md`](docs/STREAM.md). The phone fetches that over the virtual network with its own HTTP client, then decodes every frame and every audio sample itself and keeps them in sync with the audio clock.

## Run it

You need an Apple Silicon Mac with Docker (colima works) and:

```bash
brew install qemu yt-dlp ffmpeg
```

Then:

```bash
./run.sh
```

Connect a VNC viewer to `localhost:5901`. The mouse is your finger. The first run builds the kernel and userland inside an arm64 container (about 3 minutes), later runs boot in about a second.

<details>
<summary><b>Useful options and tests</b></summary>

| | |
|---|---|
| `LUMEN_DATA=path.img` | use another data disk (notes, settings, history) |
| `LUMEN_AUDIO=wav,id=snd0,path=out.wav` | capture audio to a file instead of playing it |
| `LUMEN_NO_BRIDGE=1` | start without the YouTube bridge |
| `test/e2e.py` | boots the phone headless and drives the whole flow through QMP (32 checks) |
| `test/soak.py` | 5-minute video playback soak test |
| `test/inflate_check.sh` | checks the inflate implementation against zlib |

Serial output (kernel + OS log with frame times and video stats) goes to `build/serial.log`. Touch targets for automation are listed in [`HANDOVER.md`](HANDOVER.md).
</details>

## ✦ By the numbers

| | |
|---|---|
| Own code | **8,227 lines** (about 8,000 lines of C in `src/` + the 220-line bridge) |
| Build time | about **2 hours** of Claude Code, from an empty folder ([`PROGRESS.log`](PROGRESS.log)) |
| Tests | **32 / 32** end-to-end checks passing ([report](screenshots/e2e/REPORT.txt)) |
| UI | **60 fps** median (frame work 4 to 10 ms) |
| Video | **30.0 fps**, 0 dropped frames over a 316 s soak, max A/V lag 28 ms |
| Network up | own DHCP client configures the interface in about **35 ms** |

## ✦ Written from scratch vs. third-party

Everything in `src/` was written for this project. The only third-party code:

| What | Used for |
|---|---|
| Linux 6.12 LTS kernel | drivers and TCP/IP underneath everything |
| musl libc | C library |
| `stb_truetype.h`, `stb_image.h` | glyphs, PNG/JPEG decoding (including every video frame) |
| BearSSL 0.6 | TLS for HTTPS |
| Mozilla CA bundle | certificate validation |
| Bricolage Grotesque (OFL) | the only font |

There is no JavaScript engine: the browser renders HTML and CSS only. Full details in [`docs/TECHNICAL.md`](docs/TECHNICAL.md) and [`ARCHITECTURE.md`](ARCHITECTURE.md).

## ✦ The story

This is **part 2**. In part 1, Claude built a phone OS from scratch and a lot of people watched it. The top comment said it looked like an iOS clone, and honestly it did.

So for part 2, Claude designed three completely new phone UIs (Orbit, Folio and Slab), "Slab" won, and Claude Code built the whole OS again from an empty folder, this time with real internet and YouTube. The original prompt is in [`PROMPT.md`](PROMPT.md).

<p align="center">
  <a href="https://youtu.be/ANYIBMva0wI"><img alt="Watch part 1 on YouTube" src="https://img.shields.io/badge/watch-part%201%20on%20YouTube-FF0000?style=for-the-badge&logo=youtube&logoColor=white"></a>
  <a href="https://www.youtube.com/channel/UCRsIdKfLTPW1rCOrOOfBAmA"><img alt="The Render Guy on YouTube" src="https://img.shields.io/badge/channel-The%20Render%20Guy-141414?style=for-the-badge&logo=youtube&logoColor=FF0000"></a>
</p>

## License

[MIT](LICENSE) for the Lumen OS code. Third-party components keep their own licenses: BearSSL (MIT, `third_party/bearssl/LICENSE.txt`), stb (public domain / MIT), Bricolage Grotesque (SIL OFL, `third_party/fonts/OFL.txt`), Mozilla CA bundle (MPL 2.0), Linux (GPL-2.0, not included in this repository).

<p align="center"><sub>Built by <b>Claude</b> (Anthropic) with Claude Code · made for <b>The Render Guy</b></sub></p>
