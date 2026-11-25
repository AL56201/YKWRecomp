# YKW Recomp

A static recompilation of *Yo-kai Watch 1 for Nintendo Switch* (JP, title `0100C0000CEEA000`, update **1.3.0**) into
a native Windows program. Early work in progress: the game boots and draws, nothing is playable yet.

No game files, keys or generated code are in this folder. Everything is built from your own dump.

## How it works

| Piece | Where | Notes |
|---|---|---|
| NSP intake | `tools/nxextract.py` | Decrypts your base + update NSPs with your keys; writes `exefs/` and the BKTR-patched `romfs/` |
| Recompiler | `tools/gen.py`, `lifter.py`, `simd_c.py` … | From LAS Recomp (MIT): AArch64 `main` + `sdk` NSOs → one C function per guest function |
| Runtime core | `runtime/kernel.c`, `loader.c`, `dispatch.c`, `ipc.c`, `services/` | From LAS Recomp: threads, sync, memory, IPC services (fs, hid, audio, time, am…) |
| Graphics | `runtime/gl/` | **New.** The game uses OpenGL through NVIDIA's driver (`subsdk0`), not NVN. That driver isn't loaded: EGL is implemented on SDL, and `eglGetProcAddress` hands out thunks (generated from Khronos `gl.xml` by `tools/gen_gl_thunks.py`) that call your PC's OpenGL 4.5 driver |
| Movies | `runtime/hle/movie.c` | LAS's `nn::movie` on FFmpeg, adapted (the game provides its own output buffers) |
| Input | `runtime/ui.c`, `runtime/hle/input.c` | Keyboard, SDL gamepad, mouse as the touch screen |
| Mods | `runtime/mods.c` + `services/fs.c` | LayeredFS: per-file RomFS replace/add, see below |

`subsdk1` (nnMedia) is not loaded either; its `nn::movie` API is the HLE above.

## Build (Windows, MSYS2 UCRT64)

Needs: MSYS2 with `mingw-w64-ucrt-x86_64-{clang,sdl3,ffmpeg}`, `make`; Python 3.12+ with
`pycryptodome capstone==5.0.9 unicorn==2.1.4 lz4 glad2`.

```powershell
python -I tools\nxextract.py --keys ..\prod.keys --base "<base>.nsp" --update "<update 1.3.0>.nsp" --out ..\extracted
```
Then, from an MSYS2 UCRT64 shell (or with `C:\msys64\ucrt64\bin;C:\msys64\usr\bin` first on PATH):
```
make gen PY=<windows python>      # ~25 s: generated/ (≈218 MB of C, 61k functions)
make -j12 PY=<windows python>     # build/ykwrecomp.exe
```
Run: `build\ykwrecomp.exe ..\extracted`. The build copies the MSYS2 DLLs it loads (SDL3, FFmpeg and theirs, ~100)
next to the exe (`make dlls` on its own), so no PATH setup is needed to run it.

The executable refuses an `exefs/main` other than the one it was generated from (build ID check), and
`tools/gen.py` refuses unknown game versions (`tools/game_addresses.py`).

## Controls

Keyboard: WASD left stick, IJKL right stick, arrows d-pad, Space A, Left Shift B, F X, R Y, Q L, E R, 1 ZL, 3 ZR,
Enter +, Backspace −, mouse = touch screen, F11 / Alt+Enter fullscreen. Gamepads map by position (east = A).

## Mods (e.g. the English translation, GameBanana 516048)

Put each mod in its own folder under `mods/` next to where you run the game:

```
mods/English Translation/romfs/...                         (the "FA" zip layout)
mods/Other/contents/0100C0000CEEA000/romfs/...              (Atmosphere layout also works)
```
Files replace or add to the game's RomFS by path; later folders (by name) win; an empty file named `disabled`
turns a mod off; `YKW_NO_MODS=1` turns all off. Nothing is copied.

The translation targets update 1.4.0; this build is for 1.3.0. Its FA release is data only (17 files: the patch
archive `yw1_1_1.fa` with the translated text and textures, two re-subtitled movies, localized CRI audio banks) and
works on 1.3.0 here: it loads through the overlay and the game shows it at the title. Movies opened by path also
check `mods/` first (`hle/movie.c`). If something 1.4.0-specific breaks later in the game, it can be handled per
file. Unpack the `.7z` so that `mods/<any name>/romfs/` exists; keep the mod itself out of any repository (its
license is CC BY-NC-ND).

## Debug switches

| Variable | Effect |
|---|---|
| `YKW_SAVE_DIR=<dir>` | Save folder (test runs: use a scratch one) |
| `YKW_GL_TRACE=<frame>` / `all` | Log every GL call of that frame / of every frame |
| `YKW_ROMFS_LOG=1` | Log RomFS files as they're first read |
| `YKW_MOVIE_LOG=1` | Log movie format queries |
| `YKW_NO_MOVIES=1` | Movies fail to open |
| `YKW_NO_MODS=1` | No mods |
| `YKW_SHADER_DUMP=<dir>` | Write every shader's source there |
| `YKW_GL_TRACE_ONLY=Tex,Framebuffer` | With `YKW_GL_TRACE`: only calls whose names contain these |
| `YKW_NO_DOF=1`, `YKW_DOF_DEBUG=1` | Depth of field off / drawn as its inputs (left: depth, focus, full blur) |
| `YKW_DOF_PROBE=<frame>` | Log the depth-of-field uniforms, sampler units and some depth-buffer values at that frame |
| `YKW_GL_NATIVE_SMOOTHSTEP=1`, `YKW_SHADER_CACHE=1`, `YKW_DIRECT_FB0=1` | Turn off the smoothstep fix / the fresh shader compiles / the offscreen framebuffer 0 |
| `YKW_SHADER_SUB="from=>to;;..."` | Text replacements in the post-effect shader (experiments) |
| `[shot] ... sharpness` (with `YKW_SHOT_EVERY`) | Mean neighbour luma difference in the middle of the 3D view: ~254 blurred, ~623 sharp in Nate's room |
| `YKW_FPS=30` / `60` | Frame rate for this run (overrides `fps`); `[fps]` lines every 5 s with frame time, worst and stutters (`YKW_FPS_LOG=0`: off) |
| `YKW_INTERP=0` / `1` | Frame interpolation for this run (overrides `interp`) |
| `YKW_VSYNC_TIMER=1` | On a 60 Hz display: pace with the plain timer instead of the presentations |
| `YKW_INTERP_LOCK=1` | Interpolating: run the game on the display's clock instead of its own timer (two steady states; can drop to 30 fps) |
| `YKW_PACING_RACE=1` | The game's own order in its vsync thread (wakes the frame pacing before counting the vsync: a hitch every 1-2 s) |
| `YKW_SETTINGS=<file>` | Use another settings file |
| `YKW_SIM_HZ=<hz>` | Interpolation test: act as if the display ran at that rate (images paced to a timer, swaps not waiting for the real display); `YKW_INTERP_LOG=1` reports images/s and late ones |
| `YKW_SHIFT_LOG=<a>-<b>`, `YKW_CAM_LOG=<a>-<b>` | Per frame in that test-tick range: how far the picture moved / the field camera's position, target and step |
| `YKW_PRESENT_LOG=<a>-<b>`, `YKW_WORK_LOG=<ms>`, `YKW_VSYNC_TRACE=1` | Interpolation and pacing traces: each image's timing, frames that waited long, which threads take each vsync |
| `YKW_TEST_CLOCK=1` | `YKW_INPUT` / `YKW_SHOT_EVERY` count thirtieths of a second instead of frames |
| `YKW_INPUT=600:A,900-3000/90:PLUS` | Scripted presses at presented frames (held 6 frames) |
| `YKW_SHOT_EVERY=<n>`, `YKW_SHOT_DIR=<dir>` | Save the game's image every n frames (half size, BMP) |

## Status (2026-11-25)

Working: NSP decrypt + BKTR RomFS; full recompile of `main` + `sdk` with no unsupported instructions; boot through SDK
and game initialisation; save file created; the game renders through host OpenGL (tested on an AMD Radeon GPU,
including its 38 GLSL programs); **the opening movie plays in full colour with subtitles and the game reaches the
title screen**, with the title's attract loop replaying the opening.

Movie note: the game gives the decoder one output buffer of its own (packed NV12, width × height × 1.5, set with
`movie::Buffer::SetDataAndCapacity`) and draws from it. `hle/movie.c` reports a packed layout and packs every frame
into that buffer; LAS's version swapped in its own buffer, which left the game drawing zeros (green).

**A new game plays:** gender and name entry, the opening playground cutscene and free roam in the Adams house, with
HUD, minimap and objectives, in English with the translation mod (6 minutes of scripted play, no crash). The name
check needed `hle/ngc.c`: the real profanity filter reads console system data (NgWord, `0100000000000823`) that
isn't in a game dump, and the SDK aborted when the name was confirmed.

Frame rate: set `fps` in `ykwrecomp.ini` to `30` (the game's own, default) or `60`; `interp = 1` adds frame
interpolation for displays faster than 60 Hz. Recommended: `fps = 60`, `interp = 1`.
- **60 is the ceiling.** The engine waits a whole number of vsyncs per frame (2 = 30 fps at boot; its frame-rate
  setters at `main+0x2beee0` / `main+0x2bf770`: `+0x7d0` base, `+0x7d4` interval) and advances its logic a fixed
  1/60 s per vsync. The float it also stores (`+0x7dc`) is read by no code, so above 60 vsyncs a second the game simply
  runs fast (a walk hit the bedroom wall). `tools/patches.py` routes the interval clamp through `hle/framerate.c`,
  which lowers it to one vsync a frame at `fps = 60`; anything above 60 (or `display`) is capped to 60.
- **Fullscreen (F11 / Alt+Enter)** keeps the desktop's rate when interpolating; without interpolation it picks a 60 Hz
  mode when the desktop isn't 60 Hz, so every frame is shown once. On a
  60 Hz display the game's vsync is each frame reaching the monitor (`glFinish` after the swap), with a timer only as
  the fallback: a plain timer drifts against the real refresh.
- **Frame interpolation** (`gl/interp.c`, `interp = 1`, on a display faster than 60 Hz): every frame's GL calls are
  recorded through the GL thunks and replayed for the display's refreshes in between, with float uniforms and vertex
  attributes blended between the last two frames (a replay at t = 1 is pixel-identical to the frame; frames with too
  little in common, such as scene changes, are shown as they are). The game stays on its own 60 Hz timer; each image
  waits for the desktop compositor's pass (`DwmFlush`: in a window the GL swap doesn't wait for the display), and
  in-betweens that have fallen behind are skipped. Measured on a 75 Hz monitor, 75 s walking from a save: 3,353
  images, 4 later than 20 ms, none later than 30, a steady 60 fps. High-refresh displays, simulated
  (`YKW_SIM_HZ`, images paced to a timer at that rate) on an AMD GPU: an in-between costs ~2.4 ms; 120 and 144 Hz
  showed every refresh with none late, 240 Hz 239.8-240 images a second with 4 late in 30 s; the game a steady 60.
- **The camera at 60 fps.** The field camera's follow runs once per vsync in a loop, but the loop skipped the follow
  for an odd vsync count (`main+0x198958`): at 30 fps the count is always 2; at 60 it is 1, so the camera stood still
  and jumped 40-150 px whenever a slow frame gave it a 2. Patched out (`CAM_FOLLOW_ODD_SITE`): at 60 fps the view
  moves half as far per frame as at 30, at the same speed.
- **A pacing race.** The game's vsync thread woke its frame pacing before it counted the vsync (`main+0x259ae0`,
  `main+0x2bf978`); woken first, the pacing read the old count and slept a whole vsync more, every second or two on a
  PC. The count now goes up first (`VSYNC_COUNT_FIRST_SITE`).

Graphics notes:
- **Depth of field blurred the whole 3D scene on AMD.** Two causes, both in `gl/gl_fixups.c`:
  1. The game calls `smoothstep(focus, full_blur, depth)` with the focus depth beyond the full-blur depth (near
     things blur). GLSL leaves `smoothstep(a, b, x)` undefined for a > b; NVIDIA (the Switch) gives the textbook
     result, AMD full blur. Every shader gets a `smoothstep` that computes on min/max of the edges and flips the
     result for reversed ones (`YKW_GL_NATIVE_SMOOTHSTEP=1`: the driver's own).
  2. AMD's on-disk shader cache: what it loaded on later launches drew the effect blurred again even with (1) in
     place, while every fresh compile was sharp. Each changed shader carries a tag unique to the launch, so the
     driver compiles them fresh at every boot (`YKW_SHADER_CACHE=1`: allow its cache).
  Measured with the screenshot sharpness metric over repeated runs from a save: blurred 254, fixed 622–636, the
  same as depth of field off, at 30 and 60 fps, on first and later launches and over a 5-minute session.
- The game draws a fixed 1920×1080 image (its `vi` layer size) into "framebuffer 0". That is an offscreen framebuffer
  per context (`gl/fb0.c`), scaled into the window on `eglSwapBuffers`, letterboxed to 16:9; the mouse-as-touch
  mapping follows the same rectangle. `YKW_DIRECT_FB0=1` draws straight into the window as before.

Next:
1. Shaders on NVIDIA/Intel, internal resolution above 1080p.
2. Interpolation: check other scenes (battles, menus, the overworld) for things the blend gets wrong.
3. Play further with the English translation in `mods/` and note anything that depends on 1.4.0.

## References

- [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) (MIT): the recompiler and runtime core this is built on
  (`LICENSE.LASRecomp`).
- [bgyss/SwitchRecomp](https://github.com/bgyss/SwitchRecomp): pipeline specs (provenance, input/output separation).
- [ILFforever/YW4Recomp](https://github.com/ILFforever/YW4Recomp) (MIT): Yo-kai Watch 4++ recomp. Same studio
  (Level-5 engine, CRI middleware, LayeredFS for its translation), but YW4 renders with NVN, so its renderer
  doesn't carry over; its engine notes (`docs/`) are the useful part.
