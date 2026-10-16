#!/usr/bin/env python3
"""Lumen OS 2 YouTube bridge (runs on the Mac, not on the phone).

The phone cannot run YouTube's player, so this helper does the YouTube part:
  * yt-dlp searches YouTube and resolves the real stream URLs,
  * ffmpeg decodes and re-encodes them into something simple:
    video = baseline JPEG frames 640x360 @ 30 fps, audio = PCM s16le 48 kHz stereo,
  * the two are interleaved into the packet format in docs/STREAM.md.
The phone fetches it over the virtual network (http://10.0.2.2:8765), decodes
every JPEG frame and audio sample itself, and keeps A/V in sync.

Python stdlib only, plus the yt-dlp and ffmpeg command line tools.
"""
import json, os, shutil, struct, subprocess, sys, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BIN = "/opt/homebrew/bin:/usr/local/bin:" + os.environ.get("PATH", "")
YTDLP = shutil.which("yt-dlp", path=BIN) or "yt-dlp"
FFMPEG = shutil.which("ffmpeg", path=BIN) or "ffmpeg"
W, H, FPS, RATE = 640, 360, 30, 48000
AUDIO_CHUNK = 1920 * 4                 # 40 ms of stereo s16
_search_cache, _url_cache, _thumb_cache = {}, {}, {}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def search(q):
    if q in _search_cache:
        return _search_cache[q]
    out = subprocess.run([YTDLP, "--flat-playlist", "-J", "--no-warnings", f"ytsearch20:{q}"],
                         capture_output=True, timeout=90)
    entries = json.loads(out.stdout or b"{}").get("entries") or []
    if not entries and out.returncode:
        raise RuntimeError(out.stderr.decode(errors="replace").strip()[-300:] or "yt-dlp search failed")
    words = {w for w in q.lower().split() if len(w) > 1}
    results = []
    for e in entries:
        if not e or not e.get("id"):
            continue
        results.append({
            "id": e["id"], "title": e.get("title") or "", "channel": e.get("channel") or e.get("uploader") or "",
            "duration": int(e.get("duration") or 0), "views": int(e.get("view_count") or 0),
            "uploaded": e.get("upload_date") or (time.strftime("%Y%m%d", time.gmtime(e["timestamp"])) if e.get("timestamp") else ""),
        })
    # gentle local re-rank: results whose title+channel contain more of the query words come first
    # (stable, so YouTube's own order is kept among equals)
    results.sort(key=lambda r: -len(words & set((r["title"] + " " + r["channel"]).lower().split())))
    if results:                         # never cache an empty answer (YouTube hiccups happen)
        _search_cache[q] = results
    return results


def thumb(vid):
    if vid not in _thumb_cache:
        req = urllib.request.Request(f"https://i.ytimg.com/vi/{vid}/mqdefault.jpg", headers={"User-Agent": "Mozilla/5.0"})
        _thumb_cache[vid] = urllib.request.urlopen(req, timeout=20).read()   # mqdefault is 320x180
    return _thumb_cache[vid]


def stream_urls(vid, h):
    key = (vid, h)
    if key not in _url_cache or time.time() - _url_cache[key][0] > 3600:
        fmt = f"bv*[height<={h}][vcodec!*=av01]+ba/b[height<={h}]/b"
        out = subprocess.run([YTDLP, "-g", "-f", fmt, "--no-warnings", f"https://www.youtube.com/watch?v={vid}"],
                             capture_output=True, text=True, timeout=90)
        urls = out.stdout.split()
        if not urls:
            raise RuntimeError(out.stderr.strip()[-300:] or "yt-dlp found no stream")
        _url_cache[key] = (time.time(), urls)
    return _url_cache[key][1]


def jpeg_frames(f):
    """Split an MJPEG byte stream into whole JPEG files by walking the marker structure."""
    buf = b""
    while True:
        chunk = os.read(f, 65536)
        if not chunk:
            return
        buf += chunk
        while True:
            start = buf.find(b"\xff\xd8")
            if start < 0:
                buf = b""
                break
            i = start + 2
            end = -1
            while i + 4 <= len(buf):                      # header segments carry lengths
                if buf[i] != 0xFF:
                    break
                marker = buf[i + 1]
                seglen = int.from_bytes(buf[i + 2:i + 4], "big")
                if marker == 0xDA:                          # start of scan: entropy data follows
                    j = i + 2 + seglen
                    while True:
                        j = buf.find(b"\xff", j)
                        if j < 0 or j + 1 >= len(buf):
                            break
                        if buf[j + 1] == 0xD9:
                            end = j + 2
                            break
                        j += 2 if buf[j + 1] == 0 or 0xD0 <= buf[j + 1] <= 0xD7 else 1
                    break
                i += 2 + seglen
            if end < 0:
                buf = buf[start:]
                break
            yield buf[start:end]
            buf = buf[end:]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log(self.address_string(), fmt % args)

    def send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        try:
            if u.path == "/health":
                self.send(200, b'{"ok":true}')
            elif u.path == "/search":
                self.send(200, json.dumps(search(q.get("q", ""))).encode())
            elif u.path == "/thumb":
                self.send(200, thumb(q["id"]), "image/jpeg")
            elif u.path == "/stream":
                self.stream(q["id"], float(q.get("t", 0)), int(q.get("h", 360)))
            else:
                self.send(404, b'{"error":"not found"}')
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            log("error:", e)
            try:
                self.send(502, json.dumps({"error": str(e)}).encode())
            except Exception:
                pass

    def stream(self, vid, t, h):
        urls = stream_urls(vid, h)
        vr, vw = os.pipe()
        ar, aw = os.pipe()
        inputs = []
        for url in urls:
            # read ~real time after a 15 s burst so ffmpeg never idles long enough for YouTube
            # to drop the connection, and reconnect if it does anyway
            inputs += ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_on_network_error", "1",
                       "-reconnect_delay_max", "5", "-rw_timeout", "15000000",
                       "-readrate", "1.1", "-readrate_initial_burst", "15", "-readrate_catchup", "2",
                       "-ss", f"{t:.3f}", "-i", url]
        amap = "1:a:0" if len(urls) > 1 else "0:a:0?"
        vf = f"scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,fps={FPS}"
        cmd = [FFMPEG, "-loglevel", "error", "-nostdin", *inputs,
               "-map", "0:v:0", "-vf", vf, "-c:v", "mjpeg", "-q:v", "5", "-pix_fmt", "yuvj420p", "-f", "mjpeg", f"pipe:{vw}",
               "-map", amap, "-ac", "2", "-ar", str(RATE), "-f", "s16le", f"pipe:{aw}"]
        proc = subprocess.Popen(cmd, pass_fds=(vw, aw))
        os.close(vw)
        os.close(aw)
        log(f"stream {vid} from {t:.1f}s ({len(urls)} source{'s' * (len(urls) > 1)})")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-lumen-stream")
        self.end_headers()
        lock = threading.Lock()
        start_ms = int(t * 1000)
        alive = [True]

        def packet(kind, pts, payload):
            with lock:
                if alive[0]:
                    self.wfile.write(kind + struct.pack("<II", pts, len(payload)) + payload)

        def pump_audio():
            n = 0
            try:
                while True:
                    data = b""
                    while len(data) < AUDIO_CHUNK:
                        c = os.read(ar, AUDIO_CHUNK - len(data))
                        if not c:
                            break
                        data += c
                    if not data:
                        return
                    packet(b"A", start_ms + n * 1000 // (RATE * 4), data)
                    n += len(data)
            except OSError:
                alive[0] = False

        try:
            self.wfile.write(b"LMS1")
            info = json.dumps({"w": W, "h": H, "fps": FPS, "rate": RATE, "channels": 2, "start": t}).encode()
            packet(b"I", start_ms, info)
            at = threading.Thread(target=pump_audio, daemon=True)
            at.start()
            for i, frame in enumerate(jpeg_frames(vr)):
                packet(b"V", start_ms + i * 1000 // FPS, frame)
                if not alive[0]:
                    break
            at.join()
            packet(b"E", 0, b"")
        except OSError:
            pass                                     # phone closed the stream (seek / stop)
        finally:
            alive[0] = False
            proc.kill()
            os.close(vr)
            os.close(ar)
            log(f"stream {vid} closed")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    log(f"Lumen YouTube bridge on 127.0.0.1:{port} (guest: http://10.0.2.2:{port}) using {YTDLP} and {FFMPEG}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
