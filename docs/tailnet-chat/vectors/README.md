# Tailnet chat test vectors

For protocol version 1 (`../protocol.md`). Every value in them is fixed, so
any implementation can reproduce them byte for byte; hex is lowercase.

| File | What |
|---|---|
| `handshake.json` | A dialler and a listener: Ed25519 seeds, keys, fingerprints, nonces, their `hello` and `auth` frames, both 149-byte transcripts, and `refused`: dialler auths a listener must refuse. |
| `messages.json` | `valid`: one of each JSON frame as the reference peer writes it. `invalid`: frames that must draw the `error` given (`code`, and `what` and `id` where present). |
| `frames.json` | Binary frames: kind, stream, sequence, payload and the whole frame; and malformed ones. |
| `voice.json` | `voice.opus` as a voice message: its `voice` frame, its binary chunks, its Opus packets (20 ms frames) in order, and the first three as call audio. |
| `webrtc.json` | WebRTC calls (section 4.11): one of each frame (`call_offer` with `webrtc` and `video`, `call_rtc` with an SDP or a candidate, `call_camera`), frames to refuse, ICE candidates with whether they are admitted, and an SDP as written and with its candidates outside the tailnet removed. Apart from `messages.json`, as a peer without `call:webrtc` (the T-Deck) knows none of these frames. |
| `voice.opus` | 1.5 s of a 440 Hz tone, Ogg/Opus, mono, 16 kHz input, 20 ms frames. |

The JSON files are written by `tools/tailnet_chat/make_vectors.py` from
fixed seeds (SHA-256 of a label) and `voice.opus`;
`tools/test_tailnet_chat.py` fails if they are stale. A JSON frame's text
is compact, UTF-8 and not ASCII-escaped, but a receiver must accept the
same object written any other way; compare parsed objects, not text,
except for the transcripts and signatures, which are exact bytes.

## How `voice.opus` was made

With the machine's FFmpeg 6.1.1 and its libopus, no code downloaded:

```sh
ffmpeg -nostdin -f lavfi -i "sine=frequency=440:sample_rate=16000:duration=1.5" \
  -ac 1 -ar 16000 -c:a libopus -b:a 24k -vbr on -application voip \
  -frame_duration 20 -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  -serial_offset 1 -f ogg voice.opus
```

Run twice it gives the same bytes on one machine; another FFmpeg or libopus
may give others. The committed file is the vector: do not regenerate it
unless it must change, and then run `make_vectors.py`. CBR (`-vbr off`)
pads its packets with Opus code 3; VBR gives code 0, one frame a packet,
which is what the device's encoder sends.
