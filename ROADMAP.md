# Roadmap

## Next

- **Advance Wars past the title.** The whole attract intro plays and the title
  screen comes up with memory identical to mGBA's (frame 615), but after Start
  the title's actors take a different path and the game falls back to the
  attract loop instead of the menu.
- **Noreturn detection** (`SWI 0`, panic routines), so a fall-through into
  another function's code can become a tail call.
- **Interpreter returns into native frames**, so validation failures are only
  ever native bugs.
- **CI**: build on every push; conformance needs a ROM, so CI reports SKIP.
- **Recompile RAM overlays.** Games copy routines (the m4a sound mixer, IRQ
  handlers) from ROM to IWRAM and run them there; today the interpreter runs
  them. Recompiling the ROM source at its RAM address would make every routine
  native and remove the interpreter from the hot path.
- **Audio.** The runtime has no mixer: Direct Sound FIFOs and the PSG channels
  are silent.
- **PPU completeness.** Affine backgrounds (modes 1/2), window and blend
  effects (BLDCNT/BLDY are not applied), mosaic, BG priority ordering.
- **Hardware IF semantics.** HBlank and VCount IF bits are set unconditionally;
  hardware gates them on the DISPSTAT enables.
- **Setup.cmd quick start** for game repos, per the house rules.
- **netlab recipe** so builds and QA run on the farm.

## Deferred

- The libmgba-backed runtime (`src/runtime_mgba.c`, `interception.c`,
  `verify.c`, `menu.cpp`) from the first prototype. It isn't built by default;
  mGBA's role now is the reference (`tools/oracle`), not the runtime.
- Pokemon FireRed as a second title (Flash 1 Mbit now exists).

## Out of scope

- Distributing generated code, ROMs or BIOS images.
