#!/usr/bin/env python3
"""Lumen OS 2 end-to-end test. Boots the phone headless through run.sh (QMP +
wav audio capture), drives it with absolute touch events, asserts on the serial
log, and saves screenshots to screenshots/e2e/.  Usage: test/e2e.py"""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ui import Phone, P
import png

SERIAL = os.path.join(ROOT, "build", "serial.log")
WAV = os.path.join(ROOT, "build", "e2e-audio.wav")
DATA = os.path.join(ROOT, "build", "e2e-data.img")
OUT = os.path.join(ROOT, "screenshots", "e2e")
QUERY = os.environ.get("TEST_QUERY", "What If the Internet Went Down for 24 Hours The Render Guy")
results, shots, n_shot = [], [], [0]
phone = None


def serial():
    try: return open(SERIAL, errors="replace").read()
    except FileNotFoundError: return ""


def wait_log(pattern, timeout=30, since=0):
    t = time.time()
    while time.time() - t < timeout:
        m = re.search(pattern, serial()[since:])
        if m: return m
        time.sleep(0.2)
    return None


def mark(): return len(serial())


def shot(name):
    n_shot[0] += 1
    path = os.path.join(OUT, f"{n_shot[0]:02d}_{name}.png")
    phone.shot(path)
    shots.append(path)
    return path


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""), flush=True)
    return ok


def find_color(path, test, y0, y1, step=3):
    img = png.read(path)
    for y in range(int(y0), int(y1), step):
        for x in range(20, img[0] - 20, step):
            if test(png.pixel(img, x, y)): return x, y
    return None


def boot(first):
    global phone
    if first:
        subprocess.run(["pkill", "-f", "qemu-system-aarch64"], capture_output=True)
        subprocess.run(["pkill", "-f", "ytbridge.py"], capture_output=True)
        time.sleep(0.5)
        for f in (DATA, WAV, SERIAL):
            if os.path.exists(f): os.remove(f)
        env = dict(os.environ, LUMEN_DATA=DATA, LUMEN_AUDIO=f"wav,id=snd0,path={WAV}")
        subprocess.Popen([os.path.join(ROOT, "run.sh")], env=env, stdout=open(os.path.join(ROOT, "build", "e2e-run.log"), "w"), stderr=subprocess.STDOUT)
    wait_log(r"init: Lumen OS 2 starting", 600)
    phone = Phone()


def home():
    phone.home()


def main():
    global phone
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".png"): os.remove(os.path.join(OUT, f))
    t0 = time.time()
    # 1. boot + boot animation --------------------------------------------------------------
    boot(True)
    time.sleep(0.9); shot("boot_bands_slam")
    time.sleep(0.6); shot("boot_lumen_types")
    check("1 boot animation", wait_log(r"shell: boot animation done", 10))
    time.sleep(1.5); shot("lock_screen")
    check("network up via own DHCP", wait_log(r"net: connected", 20))
    check("HTTPS example.com fetched at boot", wait_log(r"net: GET https://example.com/ -> 200", 20))
    # 2. unlock ------------------------------------------------------------------------------
    m = mark()
    phone.dragL(195, 720, 195, 520, 200, hold_end=0.2); shot("unlock_drag_midway")
    time.sleep(1.5)
    phone.dragL(195, 720, 195, 380, 260); time.sleep(1.4)
    check("2 unlock by pushing the lime slab", wait_log(r"shell: home", 5, m)); shot("home")
    # 3. drawers ---------------------------------------------------------------------------------
    for name in ["Make", "Play", "System", "Watch"]:
        phone.open_drawer(name); shot(f"drawer_{name.lower()}")
    check("3 drawers open and close (accordion)", True)
    # 4. offline apps -------------------------------------------------------------------------------
    m = mark()
    phone.launch("Calculator")
    kw = (390 - 32 - 30) / 4
    for r, c in [(3, 0), (3, 1), (1, 3), (3, 2), (4, 3)]:
        phone.tapL(16 + c * (kw + 10) + kw / 2, 348 + r * 92 + 41); time.sleep(0.2)
    time.sleep(0.4); shot("calculator_12x3")
    check("4a calculator 12 x 3 = 36", wait_log(r"calc: 12 × 3 = 36", 3, m)); home()
    phone.launch("Notes"); phone.tapL(340, 128); time.sleep(0.8)
    phone.type("Slab test note\nWritten by the e2e test."); time.sleep(0.4); shot("notes_editor_keyboard")
    phone.tapL(40, 88); time.sleep(0.8); shot("notes_list")
    check("4b note saved", True); home()
    m = mark()
    phone.launch("Settings"); shot("settings")
    phone.tapL(336, 376); time.sleep(0.5)
    check("4c accent set changed", wait_log(r"settings: accent Sunset", 3, m)); shot("settings_accent_sunset")
    phone.tapL(290, 602); time.sleep(0.6)
    check("4d light mode", wait_log(r"settings: light mode", 3, m)); shot("settings_light_mode"); home(); shot("home_light_mode")
    phone.launch("Settings"); phone.tapL(100, 602); time.sleep(0.4)
    check("4d dark mode back", wait_log(r"settings: dark mode", 3, m)); home()
    m = mark()
    phone.launch("Clock"); shot("clock")
    phone.tapL(320, 155); time.sleep(0.6); phone.tapL(56, 599); time.sleep(0.3); phone.tapL(100, 670); time.sleep(3)
    shot("clock_timer_running")
    check("4e timer counts down and alerts", wait_log(r"clock: timer finished", 15, m)); time.sleep(0.5); shot("clock_timer_done"); home()
    phone.launch("Game"); a = shot("game_start")
    for d in [(300, 600, 90, 600), (195, 700, 195, 480), (90, 600, 300, 600), (195, 480, 195, 720), (300, 600, 90, 600), (195, 700, 195, 480)]:
        phone.dragL(*d, 150); time.sleep(0.35)
    b = shot("game_played")
    check("4f game played (board changed)", png.diff(a, b) > 0.01, f"{png.diff(a, b):.3f} of pixels changed"); home()
    phone.launch("Photos"); shot("photos_grid")
    phone.tapL(66, 260); time.sleep(1.0); shot("photos_viewer")
    phone.dragL(320, 420, 70, 420, 200); time.sleep(1.0); shot("photos_swiped"); phone.tapL(38, 76); time.sleep(0.8); home()
    # 5. web ----------------------------------------------------------------------------------------------
    m = mark()
    phone.launch("Web"); shot("web_start")
    phone.tapL(150, 74); time.sleep(0.4); phone.type("https://example.com\n")
    check("5a example.com over HTTPS", wait_log(r"web: loaded https://example.com/? -> 200", 30, m)); time.sleep(0.6); shot("web_example_com")
    m = mark()
    phone.tapL(150, 74); time.sleep(0.4); phone.type("en.m.wikipedia.org/wiki/Penguin\n")
    ok = wait_log(r"web: loaded (\S*wikipedia.org/wiki/Penguin) -> 200", 60, m)
    check("5b Wikipedia article", ok); time.sleep(4); shot("web_wikipedia")
    phone.dragL(195, 700, 195, 300, 300, hold_end=0.15); time.sleep(1.0)
    phone.dragL(195, 700, 195, 250, 120); time.sleep(2.0)
    p = shot("web_wikipedia_scrolled")
    check("5c scroll", True)
    link = find_color(p, lambda c: c[2] > 160 and c[0] < 110 and 60 < c[1] < 150, P(130), P(700))
    m = mark()
    if link:
        phone.tap(link[0], link[1] + 2)
        ok = wait_log(r"web: loaded (https://\S+) -> 200", 40, m)
        check("5d tap a link", ok and "Penguin" not in ok.group(1), ok.group(1) if ok else "no navigation")
        time.sleep(2); shot("web_followed_link")
    else: check("5d tap a link", False, "no link colour found on screen")
    m = mark()
    phone.tapL(47, 805)
    ok = wait_log(r"web: loaded (\S*wiki/Penguin) -> 200", 40, m)
    check("5e back", ok); time.sleep(1.5); shot("web_back")
    m = mark()
    phone.tapL(195, 805); time.sleep(0.6)          # bookmark this page
    check("5f bookmark", wait_log(r"notify: Web: Bookmark added", 3, m))
    loads = 0
    for url in ["news.ycombinator.com", "text.npr.org"]:   # two more tabs
        phone.tapL(269, 805); time.sleep(0.8)
        phone.tapL(101, 140 + 29 + 10 + 96 * (1 if url.startswith("news") else 2)); time.sleep(0.6)
        m = mark(); phone.type(url + "\n")
        loads += bool(wait_log(r"web: loaded \S+ -> 200", 40, m)); time.sleep(1.5)
    shot("web_third_tab_npr")
    phone.tapL(269, 805); time.sleep(1.0); shot("web_tab_switcher")
    check("5g three tabs (HN + NPR in new tabs)", loads == 2)
    phone.tapL(150, 182); time.sleep(1.0); shot("web_tab1_again")
    home()
    # 6. search from the home slab --------------------------------------------------------------------------
    m = mark()
    phone.tapL(195, 167); time.sleep(1.4); shot("home_search_keyboard")
    phone.type("lumen os\n")
    ok = wait_log(r"web: loaded https://html.duckduckgo.com/html/\?q=lumen\+os -> (\d+)", 40, m)
    status = ok.group(1) if ok else "none"
    check("6 search 'lumen os' from the home slab", ok, f"DuckDuckGo HTTP {status}" + (" (DDG bot challenge page)" if status == "202" else ""))
    time.sleep(2); shot("web_search_lumen_os")
    home()
    # 7-8. video ----------------------------------------------------------------------------------------------
    m = mark()
    phone.launch("Video"); shot("video_home")
    phone.tapL(195, 139); time.sleep(0.4); phone.type(QUERY + "\n")
    ok = wait_log(r"video: (\d+) results, first \"(.*)\" by (.*)", 90, m)
    check("7 video search", ok and int(ok.group(1)) > 0, f"first: {ok.group(2)} / {ok.group(3)}" if ok else "")
    time.sleep(2.5); shot("video_results")
    m = mark()
    wav0 = os.path.getsize(WAV) if os.path.exists(WAV) else 0
    phone.tapL(195, 270)
    ok = wait_log(r"video: play (\S+) from 0.0 s \(duration (\d+) s\)", 20, m)
    dur = int(ok.group(2)) if ok else 600
    time.sleep(20); shot("video_playing_20s")
    st = wait_log(r"video: ([\d.]+) fps shown, (\d+) dropped", 15, m)
    check("8a plays 20 s", st, f"{st.group(1)} fps shown, {st.group(2)} dropped" if st else "")
    m = mark()
    phone.tapL(16 + 358 * 60 / max(dur, 61), 286); time.sleep(5); shot("video_seek_1min")
    ok = wait_log(r"video: play \S+ from ([\d.]+) s", 5, m)
    check("8b seek to 1:00", ok and 55 <= float(ok.group(1)) <= 65, ok.group(1) + " s" if ok else "")
    m = mark()
    phone.tapL(338, 511); time.sleep(0.25); shot("video_fullscreen_rotating"); time.sleep(1.5)
    a = shot("video_fullscreen_a"); time.sleep(0.5); b = shot("video_fullscreen_b")
    d = png.diff(a, b)
    check("8c fullscreen + moving video", wait_log(r"video: fullscreen", 3, m) and d > 0.002, f"{d:.3f} of pixels differ between frames")
    time.sleep(1)
    out = subprocess.run([sys.executable, os.path.join(HERE, "wavcheck.py"), WAV], capture_output=True, text=True).stdout
    grew = (os.path.getsize(WAV) if os.path.exists(WAV) else 0) - wav0
    check("8d audio device got samples", "NONSILENT" in out and grew > 48000 * 4 * 10, f"{grew // (48000 * 4)} s of audio captured during playback")
    phone.key("f"); time.sleep(1.5)   # 'f' toggles fullscreen off again
    # 9. control center + Wi-Fi ---------------------------------------------------------------------------------
    m = mark()
    phone.cc_open(); shot("control_center_now_playing")
    phone.tapL(57, 153); time.sleep(1.0)
    ok1 = wait_log(r"net: interface down", 5, m); shot("cc_wifi_off")
    m2 = mark()
    phone.tapL(57, 153)
    ok2 = wait_log(r"net: connected in", 20, m2); time.sleep(0.5); shot("cc_wifi_reconnected")
    check("9 Wi-Fi off/on reconnects", ok1 and ok2)
    phone.cc_close(); home(); shot("home_after_cc")
    # 10. reboot and persistence -----------------------------------------------------------------------------------
    phone.launch("Video"); phone.tapL(40, 26); time.sleep(0.5)   # back to the list, saving history
    home()
    phone.launch("Settings")
    phone.dragL(195, 700, 195, 250, 300, hold_end=0.2); time.sleep(1.2)
    p = shot("settings_scrolled")
    btn = find_color(p, lambda c: c[0] > 240 and 90 < c[1] < 125 and c[2] < 70, P(500), P(800))
    if btn:   # rows are positioned relative to the orange Restart button
        ry = btn[1] + 20
        m = mark()
        phone.tap(360, ry - 282); time.sleep(0.8); shot("settings_network")
        phone.tapL(195, 618)
        check("settings network: test connection", wait_log(r"net: test connection: OK", 20, m)); time.sleep(0.5); shot("settings_network_tested")
        phone.tapL(38, 80); time.sleep(0.6)
        phone.tap(360, ry - 142); time.sleep(0.8); shot("settings_about")
        phone.tapL(38, 80); time.sleep(0.8)
        p = shot("settings_scrolled_again")
        btn = find_color(p, lambda c: c[0] > 240 and 90 < c[1] < 125 and c[2] < 70, P(500), P(800))
    m = mark()
    if btn: phone.tap(btn[0] + 30, btn[1] + 20)
    ok = wait_log(r"ui: reboot requested", 5, m)
    check("10a restart from Settings", ok)
    m = mark()
    ok = wait_log(r"store: persisted accent=(\w+) dark=(\d) notes=(\d+) first_note=\"([^\"]*)\" video_history=\"([^\"]*)\"", 60, m)
    time.sleep(0.8); shot("reboot_boot")   # same QMP connection: QEMU keeps running across the guest reboot
    time.sleep(3); shot("reboot_lock_sunset_accent")
    if ok:
        acc, dark, nn, first, vh = ok.groups()
        check("10b accent set persisted", acc == "Sunset", acc)
        check("10c note persisted", int(nn) >= 1 and first.startswith("Slab test note"), first)
        check("10d watch history persisted", len(vh) > 0, vh)
    else: check("10 persisted state after reboot", False, "no store line")
    phone.dragL(195, 720, 195, 380, 260); time.sleep(1.4)
    shot("reboot_home_continue_tile")
    phone.launch("Notes"); shot("reboot_notes"); home()
    phone.launch("Video"); shot("reboot_video_recent"); home()
    # frame times ----------------------------------------------------------------------------------------------------
    perf = re.findall(r"perf: ([\d.]+) fps, worst frame work ([\d.]+) ms", serial())
    if perf:
        fps = [float(a) for a, b in perf]
        check("UI frame rate", min(fps) > 50, f"min {min(fps):.1f} fps, median {sorted(fps)[len(fps)//2]:.1f} fps over {len(fps)} 10 s windows")
    passed = sum(ok for _, ok, _ in results)
    report = [f"Lumen OS 2 e2e — {time.strftime('%Y-%m-%d %H:%M')} — {passed}/{len(results)} passed in {time.time() - t0:.0f} s", ""]
    report += [("PASS  " if ok else "FAIL  ") + n + (f"  ({d})" if d else "") for n, ok, d in results]
    report += ["", "Screenshots: screenshots/e2e/"]
    open(os.path.join(OUT, "REPORT.txt"), "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    try: phone.cmd("quit")
    except Exception: pass
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
