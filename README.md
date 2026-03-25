# gbarecomp

**The first static recompilation toolkit for Game Boy Advance ROMs.**

```
   ██████╗ ██████╗  █████╗     ██████╗ ███████╗ ██████╗ ██████╗ ███╗   ███╗██████╗
  ██╔════╝ ██╔══██╗██╔══██╗    ██╔══██╗██╔════╝██╔════╝██╔═══██╗████╗ ████║██╔══██╗
  ██║  ███╗██████╔╝███████║    ██████╔╝█████╗  ██║     ██║   ██║██╔████╔██║██████╔╝
  ██║   ██║██╔══██╗██╔══██║    ██╔══██╗██╔══╝  ██║     ██║   ██║██║╚██╔╝██║██╔═══╝
  ╚██████╔╝██████╔╝██║  ██║    ██║  ██║███████╗╚██████╗╚██████╔╝██║ ╚═╝ ██║██║
   ╚═════╝ ╚═════╝ ╚═╝  ╚═╝    ╚═╝  ╚═╝╚══════╝ ╚═════╝ ╚═════╝╚═╝     ╚═╝╚═╝
```

> **True static recompilation.** No emulator runs underneath. The recompiled C code IS the CPU. Memory is flat arrays, hardware is lightweight C modules, and the only runtime dependency is SDL2 for display.

## How It Works

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌──────────────┐
│  GBA ROM     │────>│  Analyze      │────>│  Translate   │────>│  C Source     │
│  (.gba)      │     │  ARM + Thumb  │     │  to C code   │     │  (.c/.h)      │
└─────────────┘     └──────────────┘     └─────────────┘     └──────┬───────┘
                                                                     │
                    ┌──────────────┐     ┌─────────────┐            │
                    │  Native bin   │<────│  Compile     │<───────────┘
                    │  (.exe/ELF)   │     │  gcc/clang   │     + Standalone
                    └──────────────┘     └─────────────┘       GBA Runtime
```

1. **Analysis** -- Recursive descent CFG with 6-phase pipeline: function discovery, prologue scanning, BX resolution, jump tables, block splitting, connected-component merging. Discovers 6,300+ functions in Advance Wars.
2. **Translation** -- Every ARM/Thumb instruction converted to C. Multi-file output (63+ source files) for parallel compilation. BL/SWI continuations properly merged.
3. **Standalone Runtime** -- Pure C implementation of GBA hardware: flat memory arrays, 4 hardware timers with cascade, DMA with VBlank/HBlank triggers, scanline-based scheduler, interrupt delivery, full BIOS HLE (Div, Sqrt, CpuSet, LZ77, RLE, BitUnPack, ArcTan, IntrWait, etc.)
4. **Interpreters** -- Built-in ARM and Thumb interpreters handle RAM code (IWRAM/EWRAM routines copied at runtime by the game's crt0)
5. **Display** -- SDL2 renderer with Mode 0/1/3/4 tiled and bitmap backgrounds, OBJ sprites, palette, 60fps

## Current Status

**Architecture: Standalone static recomp (no emulator)**

| Component | Implementation |
|-----------|---------------|
| CPU | Recompiled C (6,304 functions for Advance Wars) |
| Memory | Flat arrays (EWRAM 256KB, IWRAM 32KB, VRAM 96KB, etc.) |
| Bus | Direct array access with I/O dispatch |
| Timers | 4 hardware timers, prescaler, cascade |
| DMA | 4 channels, immediate/VBlank/HBlank/repeat |
| Interrupts | IE/IF/IME with handler dispatch via cpu_bx |
| BIOS | Full HLE: 20+ SWI implementations |
| PPU | Frame-based renderer (Mode 0/1/3/4, BG, OBJ) |
| Input | SDL2 keyboard |
| Save | SRAM auto-load/save (.sav files) |
| RAM code | ARM + Thumb interpreters for IWRAM/EWRAM |
| SoftReset | longjmp-based restart |

**Advance Wars test game:**
- 7.3MB standalone executable (SDL2 only dependency)
- Game init chain executes (24 sub-functions complete)
- Frames render at 60fps
- BIOS calls work (CpuSet, CpuFastSet for palette/OAM/VRAM)
- Sound engine initializes
- IRQ handler installed at 0x03000718 (ARM IWRAM)

**Known issues being worked:**
- Game stays in forced blank (DISPCNT=0x0080) -- IE/IME not being set by game init, investigating function call chain for the interrupt enable code path
- Some translator correctness issues found and fixed via register comparison verifier

## Bugs Found and Fixed

The register comparison verifier (runs functions through both recompiled C and Thumb interpreter, compares register output) has found several real bugs:

| Bug | Impact | Fix |
|-----|--------|-----|
| Thumb Format 2/Format 1 encoding overlap | ADD/SUB instructions silently skipped by interpreter | Check Format 2 before Format 1 |
| BL continuation blocks split into separate functions | Post-call code unreachable (func_080386E4 had 2 blocks instead of 24) | Merge BL successors in Phase 6 |
| SWI continuation blocks split | RegisterRamReset + SoftReset in separate functions | Merge SWI successors in Phase 6 |
| VBlank/HBlank IF flags gated behind DISPSTAT | Interrupts never fire if DISPSTAT IRQ enable not set | Set IF unconditionally |

## Quick Start

```bash
# Build the recompiler
cd gbarecomp
cmake -B build && cmake --build build --config Release

# Analyze a ROM
./build/gbarecomp info game.gba
./build/gbarecomp analyze game.gba --functions

# Generate C source (multi-file)
./build/gbarecomp translate game.gba -o output/ --multi

# Build the game (requires SDL2 via vcpkg)
cd output/
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# Run it
./build/Release/GAME game.gba
```

## Architecture

The generated executable is completely standalone:

```
main()
  └─ gba_run(game_entry)       ← setjmp for SoftReset
       └─ game_entry()          ← calls recompiled crt0
            └─ func_080000C0()  ← ARM crt0: set SP, BX to main
                 └─ cpu_bx()    ← dispatches to recompiled functions
                      ├─ func_XXXXXXXX()  ← recompiled game code
                      │    ├─ bus_read32()  → flat memory arrays
                      │    ├─ bus_write16() → I/O dispatch (DMA/timer/IRQ)
                      │    └─ gba_swi()     → BIOS HLE
                      └─ run_iwram_function() ← ARM/Thumb interpreter for RAM code
```

No emulator. No interpreter loop. The recompiled C code drives everything.

## Standing on the Shoulders of Giants

### Static Recompilation Pioneers
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** -- Proved static recompilation of console games is practical. The architectural inspiration.
- **[gb-recompiled](https://github.com/arcanite24/gb-recompiled)** -- Static recompiler for original Game Boy.

### References
- **[GBATEK](https://problemkaputt.de/gbatek.htm)** -- Martin Korth's GBA technical reference
- **[mGBA](https://github.com/mgba-emu/mgba)** -- Excellent GBA emulator (used in early prototype, now replaced by standalone runtime)
- **[pret](https://pret.github.io/)** -- GBA decompilation community

## Legal

`gbarecomp` does not include or distribute any copyrighted game data. Users must provide their own legally obtained ROM files.

---

*Built with Claude Code. True static recompilation -- no emulator underneath.*
