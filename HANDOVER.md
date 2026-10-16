# Lumen OS 2 — handover for recording

## Start / stop

```bash
cd ~/Documents/PhoneOS2 && ./run.sh
```

- Prints `Connect VNC to localhost:5901`. Guest screen is 720x1560, shown 1:1.
- First run builds in Docker (colima must be running: `colima start --arch aarch64 --vm-type vz --cpu 6 --memory 8`). Later runs boot in ~1 s.
- run.sh also starts the YouTube bridge (`bridge/ytbridge.py`, log in `build/bridge.log`) and stops it when QEMU exits.
- QMP socket: `build/qmp.sock` (one client at a time). Serial/OS log: `build/serial.log`.
- Stop: Ctrl-C in the run.sh terminal, or `python3 test/q.py quit`.
- Sound goes to the Mac speakers (coreaudio). Audio capture instead: `LUMEN_AUDIO="wav,id=snd0,path=out.wav" ./run.sh`.
- Fresh phone (no notes/history/settings): delete `data.img` before starting. The current `data.img` is in a clean state.
- Helper: `python3 test/q.py tap X Y | drag X0 Y0 X1 Y1 [ms] | type "text" | shot NAME` (guest pixels).

Input over QMP: `input-send-event` with abs axes 0..32767 (`x*32767/719`, `y*32767/1559`) + `btn left`.
Use drags with several intermediate moves (~16 ms apart) so springs/flicks look natural (test/q.py does this).

## Touch targets (guest pixels, 720x1560)

**Boot / lock**
- Boot animation runs ~3.2 s after QEMU starts (bands slam in, "lumen" types, numerals roll in).
- Unlock: drag the lime slab up from (360, 1329) to (360, 702). A short drag springs back.

**Home**
- Search the web slab: (360, 308) → opens Web with keyboard.
- Drawers: tabs above the open drawer stack at the top, tabs below it at the bottom.
  - With Watch open (default): Watch tab (360, 452), Make (360, 1274), Play (360, 1388), System (360, 1503).
  - Tab of an open drawer: Watch (360, 452), Make (360, 567), Play (360, 681), System (360, 796).
  - Drawer tabs can also be dragged up/down (finger-follow).
- Tiles (when their drawer is open):
  - Watch: Video (246, 617) · Web (587, 617) · Photos (133, 842)
  - Make: Notes (133, 731) · Clock (360, 731) · Calculator (587, 731)
  - Play: Game (133, 846)
  - System: Settings (133, 960)

**System chrome**
- Home gesture (any app): drag from (360, 1559) up to (360, 997). The app shrinks back into its tile.
- Control center: drag from (360, 4) down to (360, 899). Close: tap (360, 1477) or drag it up.
  - Toggles: Wi-Fi (105, 282) · Airplane (275, 282) · Dark/Light (445, 282) · Mute (615, 282)
  - Brightness slab y = 543, Volume slab y = 720 (drag across x 30..690)
  - Now Playing play/pause (624, 912) (only while a video is loaded)
- Keyboard: Esc goes home from any app.

**Video**
- Search field (360, 257) → type → Enter (or the orange "search" key). Results appear in ~2 s.
- First result (360, 498), second (360, 705). The Render Guy's video ranks first for its title + channel.
- Player (portrait, 2-line title): back to list (55, 41) · scrub bar x 30..690 at y 528 (drag; seeks on release; 1:00 ≈ (96, 528) for a 9:58 video)
  - −10 s (114, 945) · play/pause (360, 945) · +10 s (517, 945) · **fullscreen (624, 945)**
- Fullscreen (landscape, rotated by our compositor; video top is on the right side):
  - first tap shows the controls if they auto-hid (3.5 s)
  - play/pause (360, 780) · exit fullscreen (668, 1482) · scrub bar along x ≈ 61, y 166..1394
  - keyboard: `f` toggles fullscreen, space play/pause, ←/→ ±10 s

**Web**
- Address bar (277, 137): selects the current URL; typing replaces it; Enter loads. Words → DuckDuckGo search.
- Start page bookmarks: Wikipedia (190, 530) · Hacker News (530, 530) · CNN Lite (190, 685) · NPR Text (530, 685) · example.com (190, 840)
- Toolbar (y 1486): back (87) · forward (223) · bookmark (360) · tabs (497) · start page (633)
- Scroll: vertical drags with fling. Tap links; tap inputs to type; buttons submit forms.

**Settings**
- Accent sets (lock-screen band colours): row 1 y 694 at x 369 / 495 / 620, row 2 y 871 at x 369 / 495.
- Dark (185, 1111) · Light (535, 1111). Scroll down for sliders, text size, Network (Test connection), About, Restart phone.

## Good to know for the video

- Everything is real: the network icon reflects DHCP state, Wi-Fi toggle really downs the interface and re-runs DHCP (~30 ms), weather on the lock screen is fetched from wttr.in.
- DuckDuckGo sometimes answers address-bar searches with its "select the ducks" bot check (HTTP 202). It renders in our browser and a human can solve it; or open `html.duckduckgo.com/html/` and use its search box (POST), which is usually accepted.
- Wikipedia: `en.m.wikipedia.org/wiki/<Article>` takes ~2–3 s (900 KB of HTML + 230 KB CSS laid out on the phone).
- Measured: UI 60 fps (frame work 4–10 ms, video fullscreen ≤ 20 ms); video 30.0 fps shown, 0 dropped in steady state; JPEG decode 2.4 ms/frame.
- Screenshots of every screen: `screenshots/e2e/` (+ `REPORT.txt`) and `screenshots/milestones/`.
