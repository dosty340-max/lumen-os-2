#!/usr/bin/env python3
"""Plays the first search result for SECONDS (default 330) and reports video stats from the serial log."""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ui import Phone
SECS = int(os.environ.get("SECONDS_TO_PLAY", "330"))
QUERY = os.environ.get("TEST_QUERY", "What If the Internet Went Down for 24 Hours The Render Guy")
SERIAL = os.path.join(ROOT, "build", "serial.log"); WAV = os.path.join(ROOT, "build", "soak-audio.wav")
subprocess.run(["pkill", "-f", "qemu-system-aarch64"]); subprocess.run(["pkill", "-f", "ytbridge.py"]); time.sleep(0.5)
for f in (SERIAL, WAV):
    if os.path.exists(f): os.remove(f)
env = dict(os.environ, LUMEN_DATA=os.path.join(ROOT, "build", "soak-data.img"), LUMEN_AUDIO=f"wav,id=snd0,path={WAV}")
subprocess.Popen([os.path.join(ROOT, "run.sh")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(6); p = Phone(); p.unlock(); p.launch("Video")
p.tapL(195, 139); time.sleep(0.4); p.type(QUERY + "\n"); time.sleep(8)
p.tapL(195, 270)
t0 = time.time()
while time.time() - t0 < SECS: time.sleep(5)
p.shot(os.path.join(ROOT, "screenshots", "e2e", "soak_end.png"))
log = open(SERIAL, errors="replace").read()
rows = re.findall(r"video: ([\d.]+) fps shown, (\d+) dropped, clock ([\d.]+) s, frame lag (-?\d+) ms, audio buffered (\d+) ms", log)
buf = re.findall(r"video: buffering at", log)
perf = re.findall(r"perf: ([\d.]+) fps, worst frame work ([\d.]+) ms", log)
steady = rows[1:]
print(f"{len(rows)} windows of 10 s, clock reached {rows[-1][2] if rows else 0} s")
print("first window (startup):", rows[0] if rows else None)
if steady:
    fps = [float(r[0]) for r in steady]; drops = sum(int(r[1]) for r in steady); lag = [abs(int(r[3])) for r in steady]
    print(f"steady state: min {min(fps):.1f} fps, mean {sum(fps)/len(fps):.2f} fps, {drops} dropped frames, max |frame lag| {max(lag)} ms, buffering events {len(buf)}")
print(f"UI: min {min(float(a) for a, b in perf):.1f} fps, worst frame {max(float(b) for a, b in perf):.1f} ms" if perf else "no perf lines")
print(subprocess.run([sys.executable, os.path.join(HERE, "wavcheck.py"), WAV], capture_output=True, text=True).stdout.splitlines()[-1])
p.cmd("quit")
