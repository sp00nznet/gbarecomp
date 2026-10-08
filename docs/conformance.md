# Conformance and the reference

Two ways of checking a recompiled build, used together.

## 1. Lockstep validation (the conformance number)

`GBA_VALIDATE=K` makes the runtime run the first K calls of every recompiled
function twice from the same machine state: once natively, once through the
built-in ARM/Thumb interpreter. Registers r0-r13 and EWRAM, IWRAM, palette,
VRAM and OAM are compared; the native result is kept. Callees run natively in
both runs, so a mismatch points at one function. Implementation:
`recomp_validate` in `src/runtime.c`, hooked from `RECOMP_ENTER`.

```
VALIDATE 08070608 FAIL r0 native=080139BD interp=00000000 (sp=03007B6C lr=0801B89D frame=431)
validate: 154/158 functions agree (native vs interpreter)
```

Details that matter:

- Interrupts are held off while validating, and a call that runs past two
  frames of cycles (a main loop, a wait for an interrupt) is abandoned by
  `longjmp` and run for real, not compared.
- The stack frame below the entry SP isn't compared: natively the real LR is
  pushed there, in the interpreter its return sentinel.
- **Either side can be wrong.** A failure is a lead, not a verdict; the
  reference below settles it. The interpreter used to treat any jump from RAM
  code into ROM as a call, which made returns past the caller fail here while
  the native code was right; it now leaves on such a jump and lets the native
  unwind deliver it (or runs it as a tail call when the target is a function
  entry). Advance Wars validates 151/151.
- **It can't see runtime bugs.** Both sides share the same memory, I/O and
  interpreter for RAM code, so a fault there (Advance Wars' interpreter skipping
  the m4a mixer) passes validation. The watchpoint and the mGBA reference find
  those.

`tools/conformance.py` runs this headless, prints the count, and fails when
more functions fail than the committed baseline allows (the file holds
`passed total`; `--update` rewrites it). It compares failures, not passes,
because how many functions get validated moves with the path a run takes. Without the ROM it
prints `conformance: skipped -- ...` and exits 0. A title repo keeps its
baseline and calls it, for example:

```
py -3 ext/gbarecomp/tools/conformance.py --exe build/b/Release/AWRE.exe \
   --rom game/aw.gba --baseline conformance_baseline.txt --input tools/title.txt
conformance: 154/158 functions agree (details: scratch/conformance.log)
```

## 2. The reference: mGBA

`tools/oracle/mgba_oracle` runs the same ROM on mGBA, headless, with the
runtime's flags (`--frames`, `--input`, `--log-every`, `--dump-at`,
`--screenshot`) and the same status line, so the two runs diff line by line.
It links a locally built libmgba and is never part of a game build.

```
cmake -S tools/oracle -B build-oracle -DMGBA_DIR=<mgba checkout with build/>
cmake --build build-oracle --config Release
build-oracle/Release/mgba_oracle game.gba --frames 640 --dump-at 615 o615.bin
AWRE.exe game.gba --headless --frames 640 --dump-at 615 r615.bin
py -3 tools/memdiff.py o615.bin r615.bin
```

`--dump-at` writes EWRAM, IWRAM, IO, PAL, VRAM, OAM raw, in that order;
`memdiff.py` lists the differing ranges per region.

Notes:

- When the two drift apart in time, line up their BIOS calls:
  `ORACLE_SWI=from,to` on the oracle and `GBA_SWI_LOG=from,to` on the
  build print each SWI with its frame, scanline, cycle counter, r0-r2 and
  lr. The cycle difference between two matching calls is what the code
  between them cost on each side; that's how Advance Wars' battle load
  turned out to be VRAM waits inside `CpuFastSet`.
- `ORACLE_WATCH=addr` prints a RAM byte each frame it changes. Advance
  Wars' 0x03001D18 is 1 while a text box waits for A, which places input
  script presses clear of the frames where a line finishes.
- Start both from the same save state. The oracle doesn't load a `.sav`, so
  delete the recompiled build's `.sav` before comparing: a save written by an
  older build sent Advance Wars down a different boot path and looked like a
  bug for an hour.
- Timing follows mGBA's cycle model but isn't cycle-exact: Advance Wars
  stays within a few frames of the oracle. An input script whose press lands
  on the frame a text box opens or finishes can go either way, so keep
  presses clear of those edges.
- The I/O region differs in harmless ways (timer counters are latched, sound
  FIFO contents, pending IF bits); look at EWRAM, PAL, VRAM and OAM first.
