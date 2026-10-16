#!/usr/bin/env python3
"""QMP helper: screenshots and touch/keyboard input for Lumen OS 2.
Coordinates are guest pixels (720x1560). Also usable from the command line:
  q.py shot NAME | tap X Y | drag X0 Y0 X1 Y1 [MS] | type TEXT | key QCODE"""
import json, os, socket, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = os.path.join(ROOT, "build", "qmp.sock")
W, H = 720, 1560

class QMP:
    def __init__(self, path=SOCK):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError: time.sleep(0.1)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                if "error" in r: raise RuntimeError(r["error"])
                return r["return"]

    def _abs(self, x, y):
        return [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}]

    def move(self, x, y): self.cmd("input-send-event", events=self._abs(x, y))
    def btn(self, down): self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])

    def tap(self, x, y, hold=0.08):
        self.move(x, y); time.sleep(0.03); self.btn(True); time.sleep(hold); self.btn(False); time.sleep(0.05)

    def drag(self, x0, y0, x1, y1, ms=300, steps=None, hold_end=0.0):
        steps = steps or max(8, ms // 16)
        self.move(x0, y0); time.sleep(0.03); self.btn(True); time.sleep(0.03)
        for i in range(1, steps + 1):
            t = i / steps
            self.move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t); time.sleep(ms / 1000 / steps)
        time.sleep(hold_end); self.btn(False); time.sleep(0.05)

    def key(self, qcode, shift=False):
        ev = lambda d: ([{"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": "shift"}}}] if shift else []) + \
                       [{"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": qcode}}}]
        self.cmd("input-send-event", events=ev(True)); time.sleep(0.02)
        self.cmd("input-send-event", events=list(reversed(ev(False)))); time.sleep(0.02)

    SYM = {" ": ("spc", 0), ".": ("dot", 0), "/": ("slash", 0), ":": ("semicolon", 1), "-": ("minus", 0),
           "=": ("equal", 0), ",": ("comma", 0), "?": ("slash", 1), "\n": ("ret", 0), "_": ("minus", 1),
           "&": ("7", 1), "'": ("apostrophe", 0), "!": ("1", 1), "*": ("8", 1), "+": ("equal", 1)}

    def type(self, text):
        for ch in text:
            if ch in self.SYM: q, sh = self.SYM[ch]; self.key(q, bool(sh))
            elif ch.isupper(): self.key(ch.lower(), True)
            else: self.key(ch)

    def shot(self, name):
        path = name if name.startswith("/") else os.path.join(ROOT, "screenshots", name + ".png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.cmd("screendump", filename=path, format="png")
        return path

if __name__ == "__main__":
    q = QMP(); a = sys.argv[1:]
    if a[0] == "shot": print(q.shot(a[1]))
    elif a[0] == "tap": q.tap(float(a[1]), float(a[2]))
    elif a[0] == "drag": q.drag(*map(float, a[1:5]), ms=int(a[5]) if len(a) > 5 else 300)
    elif a[0] == "type": q.type(a[1])
    elif a[0] == "key": q.key(a[1])
    elif a[0] == "quit": q.cmd("quit")
