"""Guest touch targets for Lumen OS 2 (logical points x 1.846 = guest pixels)."""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q import QMP

S = 720 / 390
def P(v): return v * S

# home drawers (when the named drawer is the open one, its tiles are at these spots)
TAB_Y_COLLAPSED = {"Make": 690, "Play": 752, "System": 814}   # tab centres while Watch is open
TILES = {   # logical centres, valid while their drawer is open
    "Video": (133, 334), "Web": (318, 334), "Photos": (72, 456),
    "Notes": (72, 396), "Clock": (195, 396), "Calculator": (318, 396),
    "Game": (72, 458), "Settings": (72, 520),
}
DRAWER_OF = {"Video": "Watch", "Web": "Watch", "Photos": "Watch", "Notes": "Make", "Clock": "Make",
             "Calculator": "Make", "Game": "Play", "Settings": "System"}
TAB_TOP_WHEN_OPEN = {"Watch": 214, "Make": 276, "Play": 338, "System": 400}

class Phone(QMP):
    def tapL(self, x, y, **k): self.tap(P(x), P(y), **k)
    def dragL(self, x0, y0, x1, y1, ms=300, **k): self.drag(P(x0), P(y0), P(x1), P(y1), ms, **k)
    def unlock(self): self.dragL(195, 720, 195, 380, 260); time.sleep(1.3)
    def home(self): self.drag(360, 1556, 360, 1000, 260); time.sleep(1.2)
    def open_drawer(self, name):
        # tap its tab: tabs below the open drawer sit at the bottom, tabs above at the top
        order = ["Watch", "Make", "Play", "System"]
        i = order.index(name)
        # bring everything to a known state: open Watch first (its tab is always at y=214..276)
        self.tapL(195, 240); time.sleep(0.9)
        if name != "Watch":
            self.tapL(195, 845 - (4 - i) * 62 + 31); time.sleep(0.9)
    def launch(self, app):
        self.open_drawer(DRAWER_OF[app]); x, y = TILES[app]; self.tapL(x, y); time.sleep(1.2)
    def cc_open(self): self.drag(360, 4, 360, 900, 300); time.sleep(1.0)
    def cc_close(self): self.tapL(195, 800); time.sleep(1.0)
