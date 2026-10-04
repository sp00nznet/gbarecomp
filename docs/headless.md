# Running headless

Every game built with gbarecomp takes the same runtime flags, parsed once in
`src/display.c` (`display_parse_args`), so a title gets them for free.

| Flag | Effect |
|---|---|
| `--headless` | No window, no SDL video init, no 60 Hz pacing: runs as fast as it can. Works over RDP and never takes a screen. |
| `--record out.mp4` | Implies `--headless`. Pipes every frame to `ffmpeg` (must be on `PATH`), scaled 3x nearest-neighbour. |
| `--frames N` | Exit after N frames. With `--screenshot`, that's the frame saved. |
| `--screenshot out.bmp` | Save the last frame (at exit, or at frame N) as a 240x160 BMP. |
| `--input script.txt` | Scripted buttons, see below. |
| `--log-every K` | Print the frame status line every K frames (default 300; 0 = first 5 frames only). |
| `--dump-at N out.bin` | Snapshot memory at frame N (repeatable, up to 16). Layout in [conformance.md](conformance.md). |

The ROM is the first argument that isn't a flag:

```
AWRE.exe game.gba --headless --frames 600 --screenshot f600.bmp
AWRE.exe game.gba --record intro.mp4 --frames 1800
```

## Input scripts

One line per change, `<frame> <keymask hex>`; the mask holds from that frame
until the next line. Bits are KEYINPUT order with 1 = pressed:

| Bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Button | A | B | Select | Start | Right | Left | Up | Down | R | L |

```
# press Start for 5 frames at 900, then A at 1000
900 008
905 000
1000 001
1004 000
```

`tools/oracle/mgba_oracle` reads the same format, so a script drives the
recompiled build and the reference identically.

## Environment variables (diagnostics)

| Variable | Effect |
|---|---|
| `GBA_VALIDATE=K` | Lockstep-validate the first K calls of every recompiled function ([conformance.md](conformance.md)). |
| `GBA_FUNC_TRACE=1` | At exit, print the distinct functions among the last 4096 entered, with counts: "where is it stuck". |
| `GBA_TRACE_INTERP=N` | Log the first N entries into the interpreter (RAM code, and any ROM code missing from the dispatch table). |

The frame status line is

```
[frame 300] DISPCNT=0x3F40 mode=0 BG=1111 OBJ=1 BLDCNT=0x009F BLDY=0 pal0=0x0000 pal1=0x0000
```

and `mgba_oracle` prints the same line, so the two diff directly.
