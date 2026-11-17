#!/usr/bin/env python3
"""Serve a generated audiovisual clip through the production bridge protocol."""
import signal
import subprocess
import sys
import tempfile
from pathlib import Path
from http.server import ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
from ytbridge import FFMPEG, FPS, H, RATE, W, Handler, log

CLIP = {
    "id": "lumenfixture",
    "title": "A deterministic Lumen video with a deliberately long title that wraps over several lines in the player",
    "channel": "Lumen fixture",
    "duration": 120,
    "views": 0,
    "uploaded": "20260101",
}


class FixtureHandler(Handler):
    def search(self, query):
        return [CLIP] if query.lower() == "lumen video fixture" else []

    def thumb(self, vid):
        self.check_id(vid)
        return self.server.thumbnail

    def stream_inputs(self, vid, t, h):
        self.check_id(vid)
        return ["-readrate", "1.1", "-readrate_initial_burst", "3",
                "-ss", str(t), "-i", str(self.server.clip)], "0:a:0"

    @staticmethod
    def check_id(vid):
        if vid != CLIP["id"]:
            raise ValueError("unknown fixture video")


def stop(_signum, _frame):
    raise SystemExit(0)


def main():
    signal.signal(signal.SIGTERM, stop)
    with tempfile.TemporaryDirectory(prefix="lumen-video-") as directory:
        clip = Path(directory) / "fixture.mp4"
        subprocess.run([
            FFMPEG, "-loglevel", "error", "-nostdin",
            "-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={FPS}",
            "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate={RATE}",
            "-t", str(CLIP["duration"]), "-c:v", "libx264", "-preset", "ultrafast",
            "-threads", "2", "-c:a", "aac", str(clip),
        ], check=True)
        thumbnail = subprocess.check_output([
            FFMPEG, "-loglevel", "error", "-nostdin", "-i", str(clip),
            "-frames:v", "1", "-c:v", "mjpeg", "-f", "mjpeg", "pipe:1",
        ])
        with ThreadingHTTPServer(("127.0.0.1", 8765), FixtureHandler) as server:
            server.clip = clip
            server.thumbnail = thumbnail
            log("Lumen local video fixture on 127.0.0.1:8765")
            server.serve_forever()


if __name__ == "__main__":
    main()
