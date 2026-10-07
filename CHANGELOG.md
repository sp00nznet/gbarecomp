# Changelog

All notable changes to gbarecomp. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow SemVer once the first one is tagged.

## [Unreleased]

### Added
- PPU windows: WIN0, WIN1 and the OBJ window mask layers and colour effects per pixel, with wrap-around ranges as mGBA does. (#9)

### Changed
- HBlank fires at cycle 960 of each line with VCOUNT still on that line, as on hardware: the line is drawn, then HBlank DMA and IRQs run; VCOUNT, VBlank and VCount match advance at the line end. Before, HBlank ran after VCOUNT had moved on, so a handler setting up the next line from VCOUNT landed a line late (one junk line at Advance Wars' dialog box edge). HBlank DMA no longer runs for line 227 and now runs for line 159. (#10)
- The PPU renders per scanline at HBlank with the registers as they are then, composes layers by BG/OBJ priority, applies BLDCNT alpha/brighten/darken, and draws affine BGs (modes 1-2). Mosaic, affine sprites and mode 5 are still missing.
- `tools/conformance.py` fails on more failing functions than the baseline (`passed total`), not on fewer passes.

### Added
- Headless-only builds: without SDL2 the generated project builds with `GBA_NO_SDL` instead of failing, so a build farm without SDL2 can build and run QA. The runtime no longer uses SDL (libm for its trig, pacing moved to the display module).
- Diagnostics: `GBA_WATCH` write watchpoint, `GBA_FUNC_TRACE_AT` per-frame call histogram, `GBA_TRACE_UNWIND`, and `GBA_VALIDATE_RANGE`/`_FROM`/`_EVERY` to aim the validator ([docs/headless.md](docs/headless.md)).
- Runtime flags shared by every title: `--headless`, `--record out.mp4` (frames
  piped to ffmpeg), `--frames N`, `--screenshot out.bmp`, `--input script.txt`,
  `--log-every K`, `--dump-at N out.bin` ([docs/headless.md](docs/headless.md)). (#3)
- Flash backup (`FLASH_V`, `FLASH512_V`, `FLASH1M_V`): ID mode, byte program,
  sector/chip erase and 1 Mbit bank switching, saved to the same `.sav`. (#4)
- Thumb jump-table recognition, emitted as a C `switch` over local labels. (#3)
- Code-pointer scan (analysis Phase 4b): callback targets and prologue-less
  leaf functions found in ROM data become recompiled functions. (#3)
- `--entries <file>` for a title's extra entry points. (#3)
- `tools/oracle/mgba_oracle`: headless reference runs on mGBA with the same
  flags and status line as the runtime, plus memory dumps. (#3)
- `tools/memdiff.py`: region-by-region diff of two memory dumps. (#3)
- Lockstep validation (`GBA_VALIDATE=K`): each function's first K calls run
  natively and in the interpreter from one snapshot, compared, with a
  pass count at exit ([docs/conformance.md](docs/conformance.md)). (#3)
- `tools/conformance.py`: runs the validator headless and fails on a drop
  below the title's committed baseline; SKIP without the ROM. (#3)
- Return-target tracking (`RECOMP_RETURN` / `RECOMP_CALLED`): a return that
  pops an outer frame (shared epilogues, `longjmp`) now unwinds the C stack to
  the frame it belongs to instead of leaving the GBA stack misaligned. (#3)
- `GBA_FUNC_TRACE`, `GBA_TRACE_INTERP` diagnostics. (#3)
- LICENSE (MIT), CHANGELOG, ROADMAP, `docs/`. (#1)

### Fixed
- Interpreter: a jump from RAM code to ROM without a RAM return address is a return or tail call, not a call; `UMULL`/`UMLAL`/`SMULL`/`SMLAL`; `ADD rX, pc; BX rX` into ARM is now analyzed; `SWI 0` is treated as never returning.
- Two functions sharing a switch: Phase 6 gave each block to one owner, leaving the other function's cases to the interpreter. Blocks are now shared, and jump-table edges are followed.
- m4a sound driver dead from boot: SoundMain was split at a `PUSH` the prologue scan made a function of and returned holding its lock; the interpreter skipped the RAM mixer because its first bytes look like ASCII; m4a calls through the `BX r3` of another function's epilogue. Every later song start/stop was ignored, which sent the title screen back to the attract loop.
- `MOV pc, lr` emitted as a call to the caller's continuation instead of a return. (#3)
- `BX rN` falling through to the next instruction after the callee returned. (#3)
- Thumb `BX pc` (Thumb->ARM veneer) dispatching on a stale r[15]. (#3)
- ARM `LDM`/`STM` in decrementing modes storing registers in reverse order;
  `LDM {.., pc}` not returning. (#3)
- Stub functions truncated at 32 blocks and missing jump-table cases. (#3)
- Functions owning code below their entry starting at their lowest block
  instead of the entry. (#3)
- Code pointers into analyzed code splitting their host function (now
  registered after Phase 6 and copied by Phase 7). (#3)
- The interpreter leaving its return sentinel (`0xDEAD0001`) in LR or r0 for
  native code to find. (#3)
- `cpu_bx` silently dropping indirect calls nested more than 10 deep. (#3)
- `MOV pc, rN` and `ADD pc, rN` losing the Thumb bit when dispatched. (#3)
- Generated projects with very large functions taking 35+ minutes under MSVC
  (functions over 1500 instructions now compile unoptimized: 28 s total). (#3)

### Removed
- Advance Wars entry points hard-coded in the analyzer (moved to the game repo's
  entries file, passed with `--entries`). (#3)
