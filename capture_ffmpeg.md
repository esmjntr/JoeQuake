
# Video capture (branch guide)

This document is for reviewers and testers checking out **this branch**. It describes what changed in JoeQuake’s demo/video capture stack, bugs fixed in the original AVI path, and how to use the new **`capture_mode`** options.

---

## About this branch

This branch extends the existing **`capture_avi`** pipeline with direct **FFmpeg** encoding: you can create **MP4** or **MKV** files with modern codecs without writing huge uncompressed files to disk and encoding afterward. With **`capturedemo`**, video is produced as fast as the machine can render and encode (in testing, about **3×** real time for **1440p @ 60 fps** with GPU encoding).

It also fixes several bugs in the original **AVI** capture path (audio sync, sound after capture, and related issues — see **Bug fixes** below).

Note: AI tools were used to assist with creating this branch (Composer 2 & Opus 4.7).

---

## Capture modes

Set **`capture_mode`** to choose the pipeline:

| Value | Description |
|-------|-------------|
| **`legacy`** *(default)* | Original behavior: AVI via Video for Windows when `capture_avi` is `1`, otherwise a numbered TGA sequence under `capture_dir`. Uses `capture_codec`, `capture_mp3`, `capture_avi_split`, etc. |
| **`raw`** | Uncompressed **RGB24** + **stereo s16le PCM** sidecars for your own offline `ffmpeg` command. |
| **`ffmpeg`** | Same RGB + PCM stream piped into **`ffmpeg.exe`** → **`stem.mp4`** or **`stem.mkv`** in real time. **Win32 only.** |

Unknown `capture_mode` values log a warning and fall back to **`legacy`**.

---

## Quick start (`ffmpeg` mode)

1. Copy a recent **`ffmpeg.exe`** next to the game executable (e.g. beside `joequake-gl.exe`).
2. In-game or in `autoexec.cfg`:

   ```
   capture_mode ffmpeg
   capture_fps 60
   capture_dir capture
   ```

3. Record a demo:

   ```
   capturedemo mydemo
   ```

   Or start/stop manually:

   ```
   capture_start mydemo
   capture_stop
   ```

4. Output: `capture/mydemo.mp4` (or `.mkv` if `capture_ffmpeg_container mkv`).

**Batch example** (exit after the demo is captured and mux finalizes):

```
+set capture_mode ffmpeg
+set capture_autoquit 1
+capturedemo run_001
```

On failure to **start** capture, the console prints errors and **`capturedemo` stops demo playback**.

---


## Bug fixes in this branch

These fixes apply to the capture pipeline in general (not only FFmpeg mode).

### Audio/video sync at odd sample rates

Captured PCM is paced to **`shm->speed / capture_fps`** using an **integer sample accumulator** (`capture_sample_remainder` in `movie.c`). Each frame receives `remainder += sample_rate`, then `samples = remainder / fps`, `remainder %= fps`.

**Before:** per-frame rounding could drift when the sample rate did not divide evenly by **`capture_fps`** — notably **11025 Hz** at **30** or **60** fps (~15 extra samples per second), which showed up as gradual A/V desync in long captures.

**After:** total samples over N frames match `N × sample_rate / fps` exactly. Applies to **legacy AVI**, **raw**, and **ffmpeg** (all use `Movie_GetSoundtime` / `Movie_AdvanceCaptureAudioSync`).

### No sound (or loud garbage) after capture ends

While capturing, the sound clock is driven at the **video frame rate**, so `paintedtime` can run far ahead of the real DirectSound DMA cursor. Stopping capture without rebasing left:

- **Silence** until real time caught up (minutes on long captures), or  
- **Loud looping garbage** from stale `s_rawsamples` / channel end times.

**Fix:** when capture stops, the engine calls **`S_ResetTime()`**, clears **`s_rawend`**, and **`S_StopAllSounds(true)`** so normal audio resumes immediately.

---

## Requirements

### `capture_mode ffmpeg` only

| Requirement | Notes |
|-------------|--------|
| **Windows** | Non-Win32 builds: `Movie_FFmpeg_Encode_Open` fails. |
| **`ffmpeg.exe`** | Must sit beside the game `.exe`. |
| **Encoders** | Your FFmpeg build must support the codecs in `capture_ffmpeg_video_args` / `capture_ffmpeg_audio_args` (defaults: **libx264** + **AAC**). Run `ffmpeg -encoders` in that folder. |

### Stream format (raw and ffmpeg)

- **Video:** packed **RGB24**, rows in **OpenGL order** (bottom row first). The pipeline applies **`vflip`** so the encoded file is upright.
- **Audio:** interleaved **stereo s16le** at the engine sample rate (`shm->speed`, often 11025, 44100, or 48000 Hz).

---

## Commands

### `capture_start <filename>`

Starts capture using the current **`capture_mode`**.

- Argument is a **stem** (no extension required for raw/ffmpeg).
- **Legacy AVI** still forces `.avi` on the name when `capture_avi 1`.
- Output paths are under **`capture_dir`** for raw/ffmpeg (see below).
- Cannot start while a previous **ffmpeg** finalize is still running.

### `capture_stop`

Stops the active capture. For **ffmpeg**, closes pipes and waits for the child to finish muxing (the game may show **“Finalizing capture, please wait…”** for up to **30 seconds** while the window keeps updating). You cannot start another capture until finalize finishes.

### `capturedemo <filename>`

1. Runs **`playdemo`** with the same argument (`.dem` extension added when omitted; **`.dz`** supported via existing playdemo dzip handling).
2. Calls **`capture_start`** with the same stem.
3. Plays the demo **as fast as the machine can render** while capturing.
4. When the demo ends naturally, prints a **statistics summary** (not printed after manual **`capture_stop`**).

If capture **cannot be started**, errors are printed and **demo playback is stopped**.

Tab completion for `capturedemo` / `playdemo` may list `.dz` files; only arguments ending in **`.dz`** use the dzip path (a separate known issue: `.dz` lookup does not search all game directories the way `.dem` does).

---

## Output files

Paths are under **`capture_dir`** (default `capture`, relative to the game base directory unless absolute).

### `capture_mode raw`

For `capture_start myclip`:

| File | Contents |
|------|----------|
| `<capture_dir>/myclip_ffmpeg_video.raw` | Raw RGB24 frames |
| `<capture_dir>/myclip_ffmpeg_audio.pcm` | Raw stereo s16le PCM |

Offline encode example (set `W`, `H`, `FPS`, `RATE`):

```bat
ffmpeg -y ^
  -f s16le -ac 2 -ar RATE -i myclip_ffmpeg_audio.pcm ^
  -f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS -i myclip_ffmpeg_video.raw ^
  -map 0:a -map 1:v -vf vflip ^
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p ^
  -c:a aac -b:a 256k ^
  myclip.mp4
```

### `capture_mode ffmpeg`

| File | Contents |
|------|----------|
| `<capture_dir>/<stem>.mp4` or `.mkv` | Muxed output (`capture_ffmpeg_container`) |
| `<capture_dir>/<stem>_ffmpeg_stderr.txt` | Full FFmpeg stderr for the session |

The console prints the exact FFmpeg command at capture start.

---

## Cvars

### Mode and shared

| Cvar | Default | Description |
|------|---------|-------------|
| **`capture_mode`** | `legacy` | `legacy` \| `raw` \| `ffmpeg` |
| **`capture_fps`** | `30` | Output frame rate; host paces capture frames at `1/fps` (clamped 10–100000). |
| **`capture_dir`** | `capture` | Output directory (relative → under game base). Not changeable while capturing. |
| **`capture_console`** | `1` | If `0`, capture pauses while the in-game console is down (unless loading plaque). |
| **`capture_autoquit`** | `0` | If `1`, run `quit` after capture ends (stop, demo end, or encode abort). |

### Legacy only (`capture_mode legacy`)

| Cvar | Role |
|------|------|
| **`capture_avi`** | `1` = AVI, `0` = TGA sequence |
| **`capture_codec`** | AVI video fourCC (`0` = uncompressed) |
| **`capture_mp3`** / **`capture_mp3_kbps`** | Optional MP3 audio in AVI |
| **`capture_avi_split`** | Split AVI near 2 GB (default **1900** MB; `0` = off) |

Legacy does **not** use `capture_ffmpeg_*` cvars.

### FFmpeg pipe and mux (`capture_mode ffmpeg` only)

| Cvar | Default | Description |
|------|---------|-------------|
| **`capture_ffmpeg_video_buf_mb`** | `32` | Video pipe buffer (MB), clamped 1–256 |
| **`capture_ffmpeg_audio_buf_mb`** | `4` | Audio pipe buffer (MB), clamped 1–64 |
| **`capture_ffmpeg_loglevel`** | `error` | Passed to FFmpeg as `-loglevel` |
| **`capture_ffmpeg_report`** | `0` | Non-zero adds `-report` |
| **`capture_ffmpeg_write_timeout_ms`** | `5000` | Per-write timeout; abort on expiry (100–60000) |
| **`capture_ffmpeg_container`** | `mp4` | `mp4` or `mkv` only |
| **`capture_ffmpeg_video_args`** | see below | Encoder string after `-vf vflip -shortest` |
| **`capture_ffmpeg_audio_args`** | see below | Encoder string before output filename |

**`capture_ffmpeg_video_args`**

- Stripped of leading/trailing whitespace; empty → built-in default:  
  `-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p`
- Max length ~**6140** characters.

**`capture_ffmpeg_audio_args`**

- Empty default: `-c:a aac -b:a 256k -ar 48000`
- The game already passes **`-ar <engine rate>`** on the **input** side; the `-ar` in audio args affects the **encoder** output.

---

## Encoder examples (`capture_ffmpeg_video_args`)

Strings are pasted **verbatim** after **`-vf vflip -shortest`**. Wrap the whole value in quotes in the console or `autoexec.cfg` when it contains spaces.

**Check your build:** `ffmpeg -encoders` in the game directory.

**Tips**

- **H.265 / AV1 + MP4** may fail depending on FFmpeg and flags — try **`capture_ffmpeg_container mkv`** if mux errors appear in `*_ffmpeg_stderr.txt`.
- **Tune `cq`, `crf`, `global_quality`, `qp*`** to taste; lower often means higher quality / larger files (not comparable across codecs).
- **“Fast”** presets reduce CPU/GPU load during **`capturedemo`**; **“high quality”** presets may not keep up with high **`capture_fps`** on slower hardware.
- **NVENC** preset names (`p1`…`p7`, etc.) vary by FFmpeg version — if one fails, read stderr and try **`p5`** / **`p4`**.

### Software (CPU) — widest compatibility

| Goal | Typical `capture_ffmpeg_video_args` |
|------|-------------------------------------|
| **Balanced default** *(engine default)* | `-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p` |
| **Fast preview / smallest CPU** | `-c:v libx264 -preset ultrafast -crf 24 -pix_fmt yuv420p` |
| **Higher quality x264** | `-c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p` |

**x265 (HEVC, smaller files, heavier encode)**

| Goal | Typical string |
|------|----------------|
| **Balanced** | `-c:v libx265 -preset medium -crf 22 -pix_fmt yuv420p` |
| **Faster / draft** | `-c:v libx265 -preset fast -crf 26 -pix_fmt yuv420p` |
| **Higher quality** | `-c:v libx265 -preset slow -crf 18 -pix_fmt yuv420p` |

**AV1 — software**

| Encoder | Faster / drafts | Higher quality |
|---------|-----------------|----------------|
| **SVT-AV1** (`libsvtav1`) | `-c:v libsvtav1 -preset 10 -crf 32 -pix_fmt yuv420p` | `-c:v libsvtav1 -preset 6 -crf 26 -pix_fmt yuv420p` |
| **AOM** (`libaom-av1`) | `-c:v libaom-av1 -cpu-used 8 -crf 34 -pix_fmt yuv420p` | `-c:v libaom-av1 -cpu-used 4 -crf 28 -pix_fmt yuv420p` |

SVT-AV1’s `-preset` numbering is **encoder-specific** across FFmpeg versions — run `ffmpeg -h encoder=libsvtav1` and use faster presets if encode cannot keep up with **`capture_fps`**.

### NVIDIA (NVENC)

Requires recent GPU + driver + FFmpeg NVENC build. Preset names vary; if `-preset` errors, try **`p5`** / **`p4`**.

**H.264**

| Goal | Typical string |
|------|----------------|
| **Balanced** | `-c:v h264_nvenc -preset p5 -tune hq -rc vbr -cq 21 -b:v 0 -pix_fmt yuv420p` |
| **Faster capture** | `-c:v h264_nvenc -preset p3 -tune ll -rc vbr -cq 24 -b:v 0 -pix_fmt yuv420p` |
| **Higher quality** | `-c:v h264_nvenc -preset p7 -tune hq -rc vbr -cq 18 -b:v 0 -pix_fmt yuv420p` |

**HEVC**

| Goal | Typical string |
|------|----------------|
| **Balanced** | `-c:v hevc_nvenc -preset p5 -tune hq -rc vbr -cq 23 -b:v 0 -pix_fmt yuv420p` |
| **Higher quality** | `-c:v hevc_nvenc -preset p7 -tune hq -rc vbr -cq 20 -b:v 0 -pix_fmt yuv420p` |

**AV1** *(newer GPUs; often **mkv** is safer)*

| Goal | Typical string |
|------|----------------|
| **Balanced** | `-c:v av1_nvenc -preset p5 -rc vbr -cq 24 -b:v 0 -pix_fmt yuv420p` |
| **Faster** | `-c:v av1_nvenc -preset p3 -rc vbr -cq 28 -b:v 0 -pix_fmt yuv420p` |

### AMD (AMF)

Windows; requires FFmpeg with `*_amf` and current drivers.

| Codec | Faster / realtime | Higher quality |
|-------|-------------------|----------------|
| **H.264** | `-c:v h264_amf -usage lowlatency -quality speed -pix_fmt yuv420p` | `-c:v h264_amf -usage transcoding -quality quality -pix_fmt yuv420p` |
| **HEVC** | `-c:v hevc_amf -usage lowlatency -quality speed -pix_fmt yuv420p` | `-c:v hevc_amf -usage transcoding -quality quality -pix_fmt yuv420p` |

**AV1** *(hardware dependent; try **mkv** if mp4 rejects)*

`-c:v av1_amf -usage transcoding -quality balanced -pix_fmt yuv420p`

### Intel (Quick Sync / QSV)

Requires Intel iGPU/dGPU with FFmpeg **qsv** support. If `-preset` fails, omit it or use `-preset balanced`.

| Codec | Faster | Balanced | Higher quality |
|-------|--------|----------|----------------|
| **H.264** | `-c:v h264_qsv -preset veryfast -global_quality 26 -pix_fmt yuv420p` | `-c:v h264_qsv -preset medium -global_quality 23 -pix_fmt yuv420p` | `-c:v h264_qsv -preset slow -global_quality 20 -pix_fmt yuv420p` |
| **HEVC** | `-c:v hevc_qsv -preset veryfast -global_quality 28 -pix_fmt yuv420p` | `-c:v hevc_qsv -preset medium -global_quality 25 -pix_fmt yuv420p` | `-c:v hevc_qsv -preset slower -global_quality 22 -pix_fmt yuv420p` |
| **AV1** *(newer)* | `-c:v av1_qsv -preset fast -global_quality 28 -pix_fmt yuv420p` | `-c:v av1_qsv -preset medium -global_quality 25 -pix_fmt yuv420p` | `-c:v av1_qsv -preset slow -global_quality 22 -pix_fmt yuv420p` |

If **qsv** fails (“Error initializing…” in `*_ffmpeg_stderr.txt`), use software **x264/x265** above or verify QSV is available on the active GPU.

---

## Capture statistics

After **`capturedemo`** finishes naturally:

```
capture: 1800 frames, 60.0 s @ 60 fps, 28.4 s wall (2.11x), 63.4 fps
```

- **Wall time** is while frames were being captured (before FFmpeg finalize).
- **`2.11x`** means the demo timeline at **`capture_fps`** was produced in less real time (fast machine / GPU encode).

With **`capture_mode ffmpeg`**, two extra lines report **finalize** time and **total** wall time including finalize.

Not printed if you stop early with **`capture_stop`**.

---

## Troubleshooting

### Capture fails immediately

| Symptom | Things to check |
|---------|------------------|
| `ffmpeg.exe not found` | Copy FFmpeg next to the game `.exe`. |
| `capture_mode ffmpeg failed` | Win32 build, sound on, valid resolution, encoders in args. |
| `CreateProcess ffmpeg failed` | Antivirus, permissions, corrupt `ffmpeg.exe`. |
| Unknown encoder / option | Open `<stem>_ffmpeg_stderr.txt`; fix `capture_ffmpeg_*_args`. |
| `capturedemo: capture failed, stopping demo` | Fix setup errors and retry. |

### Capture stops mid-demo

Pipe **write timeout** or FFmpeg exit triggers **“stopping capture”** and `Movie_Stop`. The demo may keep playing at **fast** speed until you disconnect — only **startup** failure stops the demo automatically.

### “Still finalizing previous capture”

Wait for the prior FFmpeg process (up to **30 s**, then force-kill). The UI should keep updating during finalize.

### Empty or broken output

- Do not delete partial `.mp4` while finalize is running.
- Check `*_ffmpeg_stderr.txt` after crashes or encoder errors.
- For HEVC/AV1 in MP4, try **`capture_ffmpeg_container mkv`**.

### No sound after capture

Should be fixed on this branch. If it regresses, confirm you are on a build that includes the **`Movie_Stop`** rebaseline (`S_ResetTime`, `s_rawend`, `S_StopAllSounds`).

### Audio/video sync

Use the accumulator fix above. To verify a **raw** capture:  
`pcm_bytes / (4 × sample_rate)` ≈ `capture_frames / capture_fps` within a few samples.

### Performance

- Lower **`capture_fps`** or use a faster encoder / hardware encoder.
- Increase **`capture_ffmpeg_video_buf_mb`** if stderr suggests pipe backpressure.
- **`capturedemo`** always runs as fast as possible; wall-clock time ≠ video length.

---

## Legacy mode (brief)

With **`capture_mode legacy`** (default):

- **`capture_avi 1`**: AVI via `avifil32`, optional **`capture_codec`** and **`capture_mp3`**.
- **`capture_avi 0`**: TGA sequence under `capture_dir`.
- **`capture_avi_split`**: split AVI near 2 GB (default **1900** MB).

This branch still benefits from **audio sync** and **post-capture sound** fixes on the legacy path.

### Legacy cvar reference

| Cvar | Default | Description |
|------|---------|-------------|
| **`capture_codec`** | `0` | AVI video fourCC (`0` = uncompressed). Examples: `divx`, `xvid`. |
| **`capture_mp3`** | `0` | If `1`, compress AVI audio as MP3. |
| **`capture_mp3_kbps`** | `128` | MP3 bitrate when **`capture_mp3`** is `1`. |

**Sound note (legacy):** Quake sounds are authored at **11 kHz**. Recording at **44 kHz** does not improve them; **22 kHz** is often enough if you resample externally. Capture supports up to **44 kHz** when the engine runs at that rate.

### Parameter completion

Press **Tab** after typing a filename for:

`playdemo`, `capture_start`, `capturedemo`, `ghost`, and related commands.

Wildcards are completed automatically (e.g. `playdemo e1m1` + Tab lists matching demos). For **`playdemo`** / **`capturedemo`**, completion also searches **`.dz`** archives when matching **`.dem`** patterns.

---
