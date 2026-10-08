# Changelog

All notable changes to gbarecomp. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow SemVer once the first one is tagged.

## [Unreleased]

### Fixed
- `BgAffineSet` and `ObjAffineSet` computed the matrix as 1/scale and used the full 16-bit angle. The BIOS multiplies by the 8.8 scale and uses the angle's high byte; they now match mGBA's HLE BIOS. (#18)
- Splitting a block for a mid-block entry (Phase 5 branch targets, Phase 7 late entries) gave the tail to only the first function holding the block. Since Phase 6 shares blocks, other holders lost it and their C fell into whatever label came next. Advance Wars' score screen skipped the code that fills its score table, drew bars from garbage and hung in the RAM sprite builder. Both sites also read the block through a pointer the array grow could move. (#17)
- Nested interrupts: an IRQ now enters IRQ mode with CPSR's I bit set (old CPSR in SPSR), and a handler that clears I takes further IRQs, as on hardware. Before, nothing could interrupt a handler. Advance Wars runs its frame from the VBlank handler with HBlank nested in, and its top-of-screen text box never appeared. (#15)
- Calls into code already analyzed as part of another function (a bogus function decoded from the bytes before the callee, or a shared tail like `_call_via_r3`) got no function of their own and compiled to empty stubs. They now become entries that Phase 7 splits out. Advance Wars' `_call_via_r3` and its map-icon loader were empty; LttP gains about 7000 functions. (#13)
- HBlank and VCount IF bits are raised only when DISPSTAT enables their IRQs (bits 4 and 5), as on hardware. Advance Wars leaves IE's HBlank bit on in battle, and its map-HUD split handler drew four junk lines at the bottom of the battle screen. (#14)
- Thumb register-offset loads and stores: the translator had LDRH and LDRSB swapped (format 8 opcode bits 11-10 are STRH, LDRSB, LDRH, LDRSH), and the interpreter had the byte and halfword forms of formats 7 and 8 crossed. Advance Wars' move range drew over the whole map. (#12)

### Changed
- VRAM waits, as in mGBA: while a line is drawn, CPU code's VRAM accesses wait for a slot the enabled background layers leave free (a 32-cycle pattern per layer type). `CpuFastSet` moves 8 words per LDMIA/STMIA burst, so it pays one internal cycle and one VRAM wait per burst plus the BIOS loop's 5 cycles. Advance Wars' battle load clears 32 KB of VRAM twice; each clear took 24054 cycles and now takes 30142 (mGBA: 30111). (#25)
- Flash programming takes time, as in mGBA: after a byte write the sector reads busy (data# polling) for 650 cycles, after a sector erase for 30000. Games poll until it settles; Advance Wars' first-boot save setup ran 3 frames fast. (#22)
- Timing follows mGBA's model. Generated code charges each block's opcode fetches (2 cycles per halfword at the cartridge's sequential wait, plus a refill); before, instructions were free and every load cost a flat 2-4 cycles, so ALU-heavy code like Advance Wars' AI turns ran up to 15 frames fast per turn. Data accesses cost what mGBA charges: RAM and I/O accesses from ROM code hide behind the prefetch buffer, cartridge accesses pay WAITCNT's waits, an LDM/POP takes one internal cycle, interpreted RAM code pays its own fetches and branch refills, and DMA pays the accesses. BIOS calls cost mGBA's HLE times (call and return, `Div`'s loop, LZ77's per-byte loop, `CpuSet`'s loop). Advance Wars now stays within 4 frames of mGBA over 25000 frames of Field Training (it was 44 ahead). `GBA_FETCH_X16` scales opcode time for calibrating. (#21)
- Headless runs only draw the frames they keep (recorded, or the `--screenshot` frame): drawing was half the run time, and Advance Wars' 10000-frame runs went from 26 s to 20 s. (#11)
- HBlank fires at cycle 960 of each line with VCOUNT still on that line, as on hardware: the line is drawn, then HBlank DMA and IRQs run; VCOUNT, VBlank and VCount match advance at the line end. Before, HBlank ran after VCOUNT had moved on, so a handler setting up the next line from VCOUNT landed a line late (one junk line at Advance Wars' dialog box edge). HBlank DMA no longer runs for line 227 and now runs for line 159. (#10)
- The PPU renders per scanline at HBlank with the registers as they are then, composes layers by BG/OBJ priority, applies BLDCNT alpha/brighten/darken, and draws affine BGs (modes 1-2). Mosaic and mode 5 are still missing.
- `tools/conformance.py` fails on more failing functions than the baseline (`passed total`), not on fewer passes.

### Added
- SWI logs carry the cycle counter, r0-r2 and lr, so matching calls on the build and the oracle show what the code between them cost; `ORACLE_WATCH=addr` prints a RAM byte each frame it changes, a game-flag timeline for placing input script presses. (#26)
- SWI timelines for chasing timing drift: `GBA_SWI_LOG=from,to` in the runtime and `ORACLE_SWI=from,to` in `tools/oracle` print each BIOS call in those frames with its scanline. (#23)
- Affine sprites: OAM rotation/scaling parameters, double-size boxes, sampled about the sprite centre as on hardware. Before, they were skipped. Advance Wars' results screen lost its "Victory!" title, score labels and rank medal. (#19)
- PPU windows: WIN0, WIN1 and the OBJ window mask layers and colour effects per pixel, with wrap-around ranges as mGBA does. (#9)
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
