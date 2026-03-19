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

> **First of its kind.** As of March 2026, gbarecomp is the first toolkit to statically recompile GBA ROMs into native executables. The N64 has [N64Recomp](https://github.com/N64Recomp/N64Recomp) (9+ games ported). The Game Boy has [gb-recompiled](https://github.com/arcanite24/gb-recompiled). Now the GBA joins them. Built from scratch in 48 hours, 28 commits, from zero to a running game with graphics on screen.

## Proof of Life

The first successfully statically recompiled GBA game -- **Advance Wars** -- running natively on Windows x64:

![Advance Wars Recompiled - Title Screen](https://raw.githubusercontent.com/sp00nznet/advancewars/master/title2.png)

*Advance Wars title screen running natively on Windows x64. Full intro sequence plays, menus are navigable, and training missions are playable. Pixel-perfect rendering by mGBA's PPU with correct colors, 60fps, keyboard input.*

## The Pitch

The N64 got [static recompilation](https://github.com/N64Recomp/N64Recomp). The Game Boy got [its own recompiler](https://github.com/arcanite24/gb-recompiled). The GBA? The GBA has been sitting there with its incredible library -- Advance Wars, Fire Emblem, Metroid, Pokemon, Golden Sun -- waiting for someone to set it free.

**This is that project.**

`gbarecomp` takes a GBA ROM, analyzes the ARM7TDMI machine code, and spits out equivalent C source that compiles to a native binary. No emulation loop. No interpreter. Just your game, running on bare metal, at whatever speed and resolution your hardware can push.

## How It Works

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐     ┌──────────────┐
│  GBA ROM     │────>│  Disassemble  │────>│  Translate   │────>│  C Source     │
│  (.gba)      │     │  ARM + Thumb  │     │  to C code   │     │  (.c/.h)      │
└─────────────┘     └──────────────┘     └─────────────┘     └──────┬───────┘
                                                                     │
                    ┌──────────────┐     ┌─────────────┐            │
                    │  Native bin   │<────│  Compile     │<───────────┘
                    │  (.exe/ELF)   │     │  gcc/clang   │     + GBA Runtime
                    └──────────────┘     └─────────────┘       (libmgba)
```

1. **Disassembly** -- Full ARM7TDMI decoder (ARM 32-bit + Thumb 16-bit, all instruction formats)
2. **Analysis** -- Recursive descent CFG, function discovery (prologue scanning, BX resolution, block splitting, connected-component merging), 6,289 functions discovered in Advance Wars
3. **Translation** -- Every instruction converted to C with condition codes, flag updates, memory bus calls. Multi-file output (63 source files) for parallel compilation
4. **Runtime** -- [libmgba](https://github.com/mgba-emu/mgba) provides pixel-perfect PPU, accurate DMA/timers/interrupts, and full GBA memory map. SDL2 for display and input
5. **Compilation** -- MSVC/gcc/clang produces a native executable. 8MB for Advance Wars

## What's Working

| Feature | Status |
|---------|--------|
| ARM instruction decoder | All ARM7TDMI types |
| Thumb instruction decoder | All 19 formats |
| Control flow analysis | Recursive descent, 6-phase pipeline |
| BX dispatch | Binary search table, 6,289 entries |
| Function boundary detection | Prologue scan + connected-component merge |
| C code generation | Multi-file, 1.1M lines for Advance Wars |
| Memory bus | Via libmgba (all GBA regions) |
| PPU rendering | Via libmgba (all modes, sprites, effects) |
| DMA | Via libmgba (all channels, all timing) |
| Display | SDL2 window, 720x480 (3x scale) |
| Input | Keyboard mapped to GBA buttons |
| Save detection | Flash/SRAM auto-detected by mGBA |
| **First game rendering** | **Advance Wars title screen** |

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

# Build the game (requires libmgba + SDL2)
cd output/
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# Run it
./build/Release/GAME game.gba
```

## Standing on the Shoulders of Giants

### Static Recompilation Pioneers
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** -- The project that proved static recompilation of console games is practical. 9+ N64 games ported including Zelda: Majora's Mask, Banjo-Kazooie, Star Fox 64. The architectural blueprint for gbarecomp.
- **[gb-recompiled](https://github.com/arcanite24/gb-recompiled)** -- Static recompiler for original Game Boy (Z80 -> C). ~98% of the GB library compiles. Showed this works for handhelds too.

### GBA Emulation
- **[mGBA](https://github.com/mgba-emu/mgba)** -- The excellent GBA emulator whose `libmgba` core powers our hardware runtime. MPL-2.0 licensed with a clean `mCore` API. Without mGBA, this project wouldn't exist.

### GBA Decompilation Community

These incredible projects have manually reverse-engineered GBA games back to compilable C source. Their work proves the GBA library can be understood at the source level. If you're working on any of these, your decomp could be a starting point for a recomp too.

| Game | Project | Status |
|------|---------|--------|
| **Zelda: The Minish Cap** | [zeldaret/tmc](https://github.com/zeldaret/tmc) | 100% complete |
| **Metroid: Zero Mission** | [metroidret/mzm](https://github.com/metroidret/mzm) | ~99.89% |
| **Pokemon Emerald** | [pret/pokeemerald](https://github.com/pret/pokeemerald) | Complete |
| **Pokemon FireRed/LeafGreen** | [pret/pokefirered](https://github.com/pret/pokefirered) | Complete |
| **Pokemon Ruby/Sapphire** | [pret/pokeruby](https://github.com/pret/pokeruby) | Complete |
| **Fire Emblem: Sacred Stones** | [FireEmblemUniverse/fireemblem8u](https://github.com/FireEmblemUniverse/fireemblem8u) | ~89% |
| **Fire Emblem: Binding Blade** | [StanHash/fe6](https://github.com/StanHash/fe6) | WIP |
| **Sonic Advance 2** | [SAT-R/sa2](https://github.com/SAT-R/sa2) | ~67%, has PC port |
| **Advance Wars** | [ketsuban/advancewars](https://github.com/ketsuban/advancewars) | Byte-matching |
| **Advance Wars 2** | [Eebit/aw2bhr](https://github.com/Eebit/aw2bhr) | WIP |
| **Kirby & The Amazing Mirror** | [jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam) | WIP |
| **Super Mario Advance 2** | [atasro2/sma2](https://github.com/atasro2/sma2) | WIP |

Track progress at [decomp.dev](https://decomp.dev/projects).

### References
- **[GBATEK](https://problemkaputt.de/gbatek.htm)** -- Martin Korth's legendary GBA technical reference
- **[pret](https://pret.github.io/)** -- The decompilation community hub
- **[decomp.me](https://decomp.me/)** -- Collaborative decompilation platform
- **[RetroReversing GBA](https://www.retroreversing.com/gba/)** -- GBA reverse engineering resources
- **[agbcc](https://github.com/pret/agbcc)** -- Reconstructed GBA C compiler for matching decomps

## Want to Recomp Your Favorite GBA Game?

That's the dream. The GBA library has over 1,500 games. Any of them could be recompiled:

```bash
gbarecomp translate my_game.gba -o my_game_src/ --multi
cd my_game_src/ && cmake -B build && cmake --build build
./build/my_game my_game.gba
```

Games with existing decompilations would be the easiest targets since their code is already well-understood. But gbarecomp is designed to work with any ROM -- no prior reverse engineering needed.

## Contributing

We need people who are excited about:
- **ARM architecture** -- The ARM7TDMI is well-documented but full of quirks
- **Binary analysis** -- Disassembly, control flow recovery, pattern matching
- **Compiler internals** -- Code generation, optimization, correctness
- **GBA internals** -- Hardware timing, PPU modes, audio mixing, DMA edge cases
- **Testing** -- ROM analysis, regression testing, compatibility tracking
- **Other games** -- Pick your favorite GBA game and try recompiling it!

Open an issue, submit a PR, or just come hang out. Every GBA game that gets recompiled is a win for preservation.

## Legal

`gbarecomp` does not include or distribute any copyrighted game data. Users must provide their own legally obtained ROM files. The recompilation tools are open source. The GBA hardware runtime is based on [mGBA](https://github.com/mgba-emu/mgba) (MPL-2.0).

---

*"The GBA library is too good to be locked behind aging hardware. Let's set it free."*

*Built with Claude Code in 48 hours. From zero to rendering in 28 commits.*
