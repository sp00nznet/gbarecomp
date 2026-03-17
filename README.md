# gbarecomp

**A static recompilation toolkit for Game Boy Advance ROMs.**

```
   ██████╗ ██████╗  █████╗     ██████╗ ███████╗ ██████╗ ██████╗ ███╗   ███╗██████╗
  ██╔════╝ ██╔══██╗██╔══██╗    ██╔══██╗██╔════╝██╔════╝██╔═══██╗████╗ ████║██╔══██╗
  ██║  ███╗██████╔╝███████║    ██████╔╝█████╗  ██║     ██║   ██║██╔████╔██║██████╔╝
  ██║   ██║██╔══██╗██╔══██║    ██╔══██╗██╔══╝  ██║     ██║   ██║██║╚██╔╝██║██╔═══╝
  ╚██████╔╝██████╔╝██║  ██║    ██║  ██║███████╗╚██████╗╚██████╔╝██║ ╚═╝ ██║██║
   ╚═════╝ ╚═════╝ ╚═╝  ╚═╝    ╚═╝  ╚═╝╚══════╝ ╚═════╝ ╚═════╝╚═╝     ╚═╝╚═╝
```

## The Pitch

The N64 got [static recompilation](https://github.com/N64Recomp/N64Recomp). The Game Boy got [its own recompiler](https://github.com/arcanite24/gb-recompiled). The GBA? The GBA has been sitting there with its incredible library -- Advance Wars, Fire Emblem, Metroid, Pokemon, Golden Sun -- waiting for someone to set it free.

**This is that project.**

`gbarecomp` takes a GBA ROM, analyzes the ARM7TDMI machine code, and spits out equivalent C source that compiles to a native binary. No emulation loop. No interpreter. Just your game, running on bare metal, at whatever speed and resolution your hardware can push.

## How Static Recompilation Works

Traditional emulation interprets every instruction at runtime. Static recompilation does the hard work upfront:

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

**Phase 1: Disassembly**
- Parse the GBA ROM header and identify entry points
- Recursively disassemble ARM (32-bit) and Thumb (16-bit) instruction streams
- Build a control flow graph, resolving branches and detecting function boundaries
- Identify jump tables and indirect branch targets through data flow analysis

**Phase 2: Translation**
- Convert each ARM/Thumb instruction to equivalent C operations
- Map the ARM7TDMI register file (R0-R15, CPSR) to C variables
- Replace memory accesses with calls to the runtime's memory bus
- Handle ARM/Thumb interworking (BX instructions that switch ISA mode)

**Phase 3: Runtime**
- Everything except the CPU is handled by the GBA runtime library
- PPU (graphics), APU (audio), DMA, timers, interrupts -- all faithfully emulated
- Built on [libmgba](https://github.com/mgba-emu/mgba), one of the most accurate GBA emulators
- Memory-mapped I/O writes dispatch to the runtime's hardware models

**Phase 4: Compilation**
- Standard C compiler (gcc, clang, MSVC) produces a native binary
- Link against the GBA runtime library
- The result runs on Windows, Linux, macOS -- anywhere you can compile C

## Architecture

```
gbarecomp/
├── disasm/          # ARM7TDMI disassembler (ARM + Thumb)
├── analysis/        # Control flow analysis, jump table detection
├── translate/       # Instruction-to-C translation engine
├── runtime/         # GBA hardware runtime (wraps libmgba)
│   ├── bus.c        # Memory bus + MMIO dispatch
│   ├── ppu.c        # Video (wraps mGBA PPU)
│   ├── apu.c        # Audio (wraps mGBA APU)
│   ├── dma.c        # DMA controller
│   ├── timer.c      # Timer subsystem
│   ├── irq.c        # Interrupt controller
│   └── save.c       # SRAM/Flash/EEPROM
├── output/          # C code generation and formatting
└── tools/           # ROM analysis utilities
```

## The Hard Problems (and How We'll Solve Them)

### ARM/Thumb Interworking
GBA code freely switches between 32-bit ARM and 16-bit Thumb modes via `BX` instructions. The lowest bit of the target address determines the mode. Our disassembler tracks mode switches through the control flow graph and translates both ISAs.

### Indirect Branches
`BX Rm` where the register value isn't known statically. This is the classic static recompilation headache. Our approach:
1. **Pattern matching** -- Detect common jump table idioms (`LDR PC, [PC, Rm, LSL #2]`)
2. **Data flow tracking** -- Trace register values backward to bound the set of possible targets
3. **Runtime fallback** -- For truly dynamic branches (rare on GBA), dispatch through a function pointer table populated at load time

### Self-Modifying Code
Some games copy routines to IWRAM and execute from there. We handle this with:
1. **Static detection** -- Identify DMA/memcpy to IWRAM followed by execution
2. **Pre-analysis** -- If the copied code is a known pattern, recompile it statically
3. **Interpreter fallback** -- For dynamic IWRAM code, fall back to an ARM interpreter (from libmgba)

### Timing Sensitivity
GBA games depend on precise DMA/timer/PPU timing. Even though the CPU runs natively, the runtime must model cycle-accurate hardware behavior. The libmgba core handles this -- we synchronize recompiled code with the runtime's cycle counter.

## GBA Hardware at a Glance

| Component | What It Does | How We Handle It |
|-----------|-------------|-----------------|
| **ARM7TDMI CPU** | 16.78 MHz, ARM + Thumb ISAs | Statically recompiled to C |
| **PPU** | 240x160, 4 BG layers, 128 sprites, 6 video modes | libmgba PPU core |
| **APU** | 4 PSG channels + 2 PCM DMA channels | libmgba APU core |
| **DMA** | 4 channels, VBlank/HBlank/FIFO triggers | libmgba DMA model |
| **Timers** | 4x 16-bit with prescaler + cascade | libmgba timer model |
| **Memory** | BIOS(16K), EWRAM(256K), IWRAM(32K), VRAM(96K), ROM(32M) | Runtime memory bus |
| **Interrupts** | VBlank, HBlank, Timer, DMA, Keypad, etc. | Runtime IRQ dispatch |
| **Saves** | SRAM, Flash, EEPROM (varies by game) | Runtime save abstraction |

## Project Status

| Component | Status |
|-----------|--------|
| ROM loader + header parsing | Not started |
| ARM disassembler | Not started |
| Thumb disassembler | Not started |
| Control flow analysis | Not started |
| Jump table detection | Not started |
| ARM -> C translation | Not started |
| Thumb -> C translation | Not started |
| Memory bus runtime | Not started |
| PPU runtime (libmgba) | Not started |
| APU runtime (libmgba) | Not started |
| DMA/Timer/IRQ runtime | Not started |
| End-to-end pipeline | Not started |

## First Target

Our first recompilation target is **[Advance Wars](https://github.com/sp00nznet/advancewars)** -- a beloved GBA strategy game that deserves to live forever. But `gbarecomp` is designed to be game-agnostic. Once the toolchain is proven, any GBA ROM is fair game.

## Standing on the Shoulders of Giants

This project wouldn't be possible without:

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** -- Proved that static recompilation of console games is not only possible but practical. The architectural blueprint for this project.
- **[gb-recompiled](https://github.com/arcanite24/gb-recompiled)** -- Showed that static recomp works for handheld games too. Their jump table analysis work is directly relevant.
- **[mGBA](https://github.com/mgba-emu/mgba)** -- Endri Lakanovic and contributors built an incredible GBA emulator with a clean library interface. We're using `libmgba` as our hardware runtime.
- **[GBATEK](https://problemkaputt.de/gbatek.htm)** -- Martin Korth's legendary GBA technical reference. The bible for anyone touching GBA hardware.
- **[pret](https://github.com/pret)** -- The decompilation community that has reverse-engineered dozens of GBA games. Their work proves these games can be understood at the source level.

## Want to Recomp Your Favorite GBA Game?

That's the dream. Once `gbarecomp` is mature enough, the workflow will be:

```bash
# Analyze the ROM
gbarecomp analyze my_game.gba

# Generate C source
gbarecomp translate my_game.gba -o my_game_src/

# Build it
cd my_game_src/
cmake -B build
cmake --build build

# Play it
./build/my_game
```

We want to make this accessible enough that anyone with a GBA ROM and a C compiler can produce a native build. If you've got a favorite GBA game you want to see recompiled, come help us build the tools to make it happen.

## Contributing

We need people who are excited about:
- **ARM architecture** -- The ARM7TDMI is well-documented but full of quirks
- **Binary analysis** -- Disassembly, control flow recovery, pattern matching
- **Compiler internals** -- Code generation, optimization, correctness
- **GBA internals** -- Hardware timing, PPU modes, audio mixing, DMA edge cases
- **Testing** -- ROM analysis, regression testing, compatibility tracking

Open an issue, submit a PR, or just come hang out. Every GBA game that gets recompiled is a win for preservation.

## Legal

`gbarecomp` does not include or distribute any copyrighted game data. Users must provide their own legally obtained ROM files. The recompilation tools are open source. The GBA hardware runtime is based on [mGBA](https://github.com/mgba-emu/mgba) (MPL-2.0).

---

*"The GBA library is too good to be locked behind aging hardware. Let's set it free."*
