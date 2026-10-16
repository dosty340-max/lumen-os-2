# Lumen stream format (`/stream`)

`GET http://10.0.2.2:8765/stream?id=VIDEO_ID&t=SECONDS&h=360`

The bridge answers `200 OK` with `Content-Type: application/x-lumen-stream` and
no `Content-Length`; the body runs until the video ends (or the phone closes the
connection, which is how seeking works: close, then reopen with a new `t`).

```
stream  := "LMS1" packet*
packet  := type:u8  pts_ms:u32le  length:u32le  payload[length]
```

| type | meaning | payload |
|------|---------|---------|
| `I`  | info, always first | UTF-8 JSON: `{"w":640,"h":360,"fps":30,"rate":48000,"channels":2,"start":T}` |
| `V`  | one video frame | a complete baseline JPEG (SOI..EOI), 640x360, letterboxed to 16:9 |
| `A`  | audio | PCM signed 16-bit little-endian, 48 kHz, stereo interleaved (L R L R ...), usually 40 ms (7680 bytes) |
| `E`  | end of stream | empty |

`pts_ms` is the presentation time in milliseconds from the start of the video
(so the first packets after a seek to `t=60` have `pts_ms` around 60000).
Video frames are 30 fps (`pts = start + i*1000/30`); audio pts is derived from
the number of samples sent. Packets of the two kinds are interleaved roughly in
time order; the phone keeps separate queues and uses the audio clock as master.

Other endpoints: `/search?q=` (JSON list of up to 20 `{id,title,channel,duration,views,uploaded}`),
`/thumb?id=` (320x180 JPEG), `/health`.
