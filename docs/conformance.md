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
- **Either side can be wrong.** The interpreter can't follow a return that
  pops an outer frame or lands in native code (it treats `BX` to a ROM
  address as a call), so those functions fail here while the native code is
  right. Advance Wars' four current failures are all of this kind
  (`_call_via_r4` into RAM flash routines, the shared-epilogue actor handlers,
  and the `BX pc` veneer into an ARM routine). A failure is a lead, not a
  verdict; the reference below settles it.

`tools/conformance.py` runs this headless, prints the count, and fails when it
drops below a committed baseline (`--update` raises it). Without the ROM it
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

- Start both from the same save state. The oracle doesn't load a `.sav`, so
  delete the recompiled build's `.sav` before comparing: a save written by an
  older build sent Advance Wars down a different boot path and looked like a
  bug for an hour.
- The recompiled build boots about 15 frames faster than hardware (its
  instruction timing is approximate), so compare by scene, not frame number,
  early on.
- The I/O region differs in harmless ways (timer counters are latched, sound
  FIFO contents, pending IF bits); look at EWRAM, PAL, VRAM and OAM first.
