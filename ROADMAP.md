# Roadmap

## Next

- **Advance Wars into Field Training** and a full battle: the menus and name
  entry now match mGBA.
- **Recompile RAM overlays.** Games copy routines (the m4a sound mixer, IRQ
  handlers, Flash routines) from ROM to IWRAM and run them there; the
  interpreter runs them today, and their calls into ROM are invisible to
  analysis (Advance Wars lists two in its entries file). Recompiling the ROM
  source at its RAM address would make every routine native.
- **Noreturn detection** beyond `SWI 0` (panic routines), so more
  fall-throughs into another function's code can become tail calls.
- **Audio.** The m4a mixer now runs, but the runtime has no output: Direct
  Sound FIFOs and the PSG channels are silent.
- **PPU completeness.** Mosaic, mode 5.
- **Setup.cmd quick start** for game repos, per the house rules.
- **netlab recipe** so builds and QA run on the farm.

## Deferred

- The libmgba-backed runtime (`src/runtime_mgba.c`, `interception.c`,
  `verify.c`, `menu.cpp`) from the first prototype. It isn't built by default;
  mGBA's role now is the reference (`tools/oracle`), not the runtime.
- Pokemon FireRed as a second title (Flash 1 Mbit now exists).

## Out of scope

- Distributing generated code, ROMs or BIOS images.
