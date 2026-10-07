# gbarecomp

A static recompiler for Game Boy Advance ROMs. It reads a ROM, finds the ARM
and Thumb code, translates it to C, and writes a CMake project that builds a
native executable against a small GBA runtime (memory map, timers, DMA,
interrupts, BIOS calls, PPU, input) with SDL2 for the window. No emulator runs
underneath: the recompiled C is the CPU.

Part of the recomp family (snesrecomp, lynxrecomp, xboxrecomp, ps3recomp,
pcrecomp, ...) and follows the same house style: toolkit and game repos kept
apart, generated code never committed, `--headless --record` on every build, a
conformance harness against a reference.

## Status

**Alpha.** One title is in progress: [Advance Wars](https://github.com/sp00nznet/advancewars).

| Title | Boots | Intro | Title screen | Menus | Gameplay | Conformance |
|---|---|---|---|---|---|---|
| Advance Wars (USA, Rev 1) | yes | full attract loop | yes | yes, matches mGBA screen for screen | Field Training: Day 1, the enemy turn and Day 2's battle, in step with mGBA | 151/151 functions |
| LttP + Four Swords (USA) | yes | - | yes | no (blanks where mGBA goes to file select) | no | - |

The conformance figure is lockstep validation: each recompiled function's
calls run natively and in the interpreter from the same state and are
compared ([docs/conformance.md](docs/conformance.md)).

Builds work with MSVC and, headless-only, on a clang-cl build farm without
SDL2. Missing: audio output (the m4a driver runs, nothing is mixed out),
mosaic in the PPU, recompiling RAM overlays, and the
other items in [ROADMAP.md](ROADMAP.md).

## Screenshots

Advance Wars, recompiled, captured with `--headless --screenshot`:

![Advance Wars: title, Nell's welcome, the Field Training briefing and the first battle map](docs/screenshots/advance-wars-menus.png)

![Advance Wars intro: map, Max, the battle scene and the logo](docs/screenshots/advance-wars-intro.png)

## Getting Started

The toolkit is used from a game repo; the game repo's `Setup.cmd` is the quick
start (it fetches this repo as a submodule, asks for your ROM and builds). To
use the toolkit directly:

1. Install Visual Studio 2022 (or its Build Tools) with *Desktop development
   with C++*, CMake 3.16+, and SDL2 through vcpkg:
   ```
   vcpkg install sdl2:x64-windows
   ```
2. Build the recompiler:
   ```
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
3. Translate a ROM you own. Run it from the toolkit folder, because it copies
   the runtime sources (`src/runtime.c`, `src/display.c`, `include/gba/*.h`)
   into the output by relative path:
   ```
   build\Release\gbarecomp.exe translate game.gba -o out --multi --entries entries.txt
   ```
   Expected tail of the output:
   ```
   [analysis] Pass 1: 456 new entries from code pointers (1488 analyzed fresh)
   ...
   Generated 78 files in: out
   Functions translated: 7381
   ```
4. Build and run the generated project:
   ```
   cmake -S out -B out/b -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
   cmake --build out/b --config Release --parallel
   out\b\Release\AWRE.exe game.gba --headless --frames 600 --screenshot f600.bmp
   ```
   Copy `SDL2.dll` from `<vcpkg>/installed/x64-windows/bin` next to the exe.

The output directory is generated from your ROM and is never committed or
distributed (see Legal).

## Usage

```
gbarecomp info <rom.gba>                         ROM header
gbarecomp disasm <rom.gba> [--start A --count N --thumb|--arm]
gbarecomp analyze <rom.gba> [--functions] [--detail A] [--entries file]
gbarecomp translate <rom.gba> -o <dir> --multi [--entries file]
```

`--entries` takes the title's extra entry points, one hex address per line
(`#` comments). Most titles need few or none: analysis finds jump tables,
code pointers in ROM data and prologue-less leaf functions on its own
([docs/analysis.md](docs/analysis.md)).

Every generated game takes the runtime flags in [docs/headless.md](docs/headless.md):
`--headless`, `--record out.mp4`, `--frames N`, `--screenshot`, `--input`,
`--log-every`, `--dump-at`, and the `GBA_VALIDATE`, `GBA_FUNC_TRACE`,
`GBA_TRACE_INTERP` diagnostics.

## Building from source

As in Getting Started. The optional reference runner needs a built
[mGBA](https://github.com/mgba-emu/mgba) checkout:

```
cmake -S tools/oracle -B build-oracle -DMGBA_DIR=<mgba>
cmake --build build-oracle --config Release
```

## Documentation

- [docs/analysis.md](docs/analysis.md): analysis phases and the translation
  pitfalls found on real code (jump tables, returns, block transfers, compile time).
- [docs/headless.md](docs/headless.md): runtime flags, input scripts, diagnostics.
- [docs/conformance.md](docs/conformance.md): lockstep validation and the mGBA reference.

## Legal

gbarecomp contains no game data and its output is never distributed: you
supply your own ROM and the generated C stays on your machine. No BIOS image is
needed or used; BIOS calls are reimplemented in `src/runtime.c`. mGBA is used
only as an optional, separately built reference tool.

## License

MIT, see [LICENSE](LICENSE).
