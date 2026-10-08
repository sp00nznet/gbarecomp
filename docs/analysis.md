# Analysis and translation notes

What `src/analysis.c` and `src/translate.c` do that isn't obvious from the code,
and why. Each item cost a debugging session on a real title (Advance Wars,
2026-10) and would cost the next reader the same.

## Analysis phases

| Phase | What | Why |
|---|---|---|
| 1 | Recursive descent from the ROM header entry, then the title's `--entries` file | Ordinary control flow and direct calls. |
| 2 | `BX Rn` resolved from a literal-pool `LDR Rn, =target` | Most indirect jumps in GCC/SDT code are this. |
| 3 | ARM jump tables (`ADD pc, pc, Rn, LSL #2`, `LDR pc, [pc, Rn, LSL #2]`) | |
| 4 | Prologue scan (`PUSH {.., lr}`, `STMDB sp!, {.., lr}`) over unvisited ROM | Functions only reached through pointers. |
| 4b | Code-pointer scan (below) | Leaf functions and callbacks Phase 4 can't see. |
| 5 | Split blocks at mid-block branch targets | |
| 6 | Merge branch-connected blocks into their function; a branch to another function's entry is a tail call | |
| 7 | Populate functions whose entry lands inside another function by duplicating the reachable blocks | A function pointer into the middle of code another function owns. |

Thumb jump tables are found inline while walking a block (`detect_thumb_jump_table`).

## Thumb switch statements

The GBA toolchains emit:

```
    CMP   rI, #N          ; possibly a block or two earlier
    BHI   default         ; or: BLS +2 / B default, when default is far away
    LSL   rI, rJ, #2      ; the bound may be checked on rJ, the LSL's source
    LDR   rT, =table
    ADD   rX, rI, rT      ; either operand order
    LDR   rX, [rX, #0]
    MOV   pc, rX
```

followed by N+1 absolute case addresses. Before this was recognised,
`MOV pc, rX` wasn't even a block end: the analyzer decoded the table as
instructions and ran on into the case bodies, and the translator emitted
`cpu_bx(case_address)`, a jump into the middle of a C function that the
dispatch table can't serve, so it fell to the interpreter and never came back
cleanly. The translator now emits a `switch` over the function's own labels,
with `cpu_bx` (Thumb bit set) only as the default.

Advance Wars has 98 such tables; the 77 in reachable code are all recognised.

## Returns that don't look like `BX lr`

- `MOV pc, lr` is a return. It was emitted as `cpu_bx(lr)`, which "called" the
  caller's continuation from inside the callee.
- `POP {rN}; BX rN` is a return (handled since early on).
- Any other `BX rN` never falls through: it ends with `return` after `cpu_bx`.
  Without it, `_call_via_r3` and friends ran straight into the next veneer once
  the callee returned. The exception is `MOV lr, pc; BX rN`, an indirect call
  that does come back.
- `BX pc` in Thumb is an interworking veneer into ARM at `(pc + 4) & ~3`: a
  tail call. It used to be `cpu_bx(r[15])`, with whatever stale value r[15]
  held.

## Code pointers (Phase 4b)

Callback tables, state machines and task lists hold Thumb addresses (odd
words) that no `BL` reaches. Every ROM word that is such a pointer becomes a
function entry when:

- it points into Thumb code already analyzed (Phase 7 then materialises it if it
  lands inside another function), or
- it points at unanalyzed bytes that follow a function end (`BX`, `POP {pc}`,
  `B`) within 12 bytes, allowing for padding and up to two literal-pool words.
  These are leaf functions with no `PUSH` prologue, which Phase 4 can't find.

The scan repeats until it finds nothing new. Pointers into code already
analyzed are only registered after Phase 6 ("late entries"), so they get their
own copy of the blocks from Phase 7 instead of becoming a function boundary
that splits their host, loops included, into tail calls. A data word that only
looks like a pointer then costs a duplicate function, never wrong behaviour:
nothing dispatches to it except `cpu_bx` with that exact address. On Advance
Wars: 456 functions found fresh, 596 late entries. Before this, dozens of ROM
routines ran in the interpreter every frame, and one of them, entered
mid-function, popped its native caller's return address and carried on
interpreting the caller.

## Function entry and block order

Blocks are emitted in address order. A function that also owns code *below*
its entry (merged predecessors, Phase 7 copies) used to start executing at its
lowest block; 1323 Advance Wars functions now open with `goto label_<entry>`.
A block that falls through to a label that isn't the next one emitted gets an
explicit `goto`. Falling through into code owned by another function still
returns, as before: tail-calling it broke boot, because the analyzer decodes
past calls that never return (`SWI 0`, panics) into literal pools that then
"fall through" into the next function. That needs noreturn detection.

## Returns that skip their caller

GBA code doesn't always return to its caller. Advance Wars' actor handlers
share an epilogue with a wrapper: the wrapper pushes `{r4, lr}` and jumps to
the handler through `_call_via_r4`, and the handler ends with
`POP {r4}; POP {r0}; BX r0`, popping the wrapper's frame and returning straight
to the wrapper's caller. `setjmp`/`longjmp` (AW has an ARM pair at
`0x080001D0`/`0x080001E4`) do the same across many frames.

With C returns, the handler returned into the wrapper, which then popped two
more words: the GBA stack ended up 8 bytes off. Now every return records where
it really goes (`RECOMP_RETURN` sets `g_ret`) and every call site checks it
(`RECOMP_CALLED`): if it isn't this site's continuation, the C frame returns
too, and the unwind continues to the frame it belongs to. Interrupts save and
restore `g_ret`, and the interpreter clears it after calling native code.

`longjmp` needs one more step: it goes back to the `setjmp` call's
continuation, but the function that called `setjmp` has moved on, so the
unwind meets it at some other call site. A function with a Thumb `BL` to
setjmp (recognized by its `STMIA r0!, {.., lr}`, through a `BX pc` veneer and
a `B`) checks after every call whether the return is headed for that
continuation and jumps there. All CPU state lives in `r[]`, so that `goto` is
the whole resume. Advance Wars' AI calls `setjmp` (at `0x080643CA`), then
longjmps back from deep in its search (`0x0806461A`). Its `setjmp` and `longjmp` end in
`TST lr, #1; MOVEQ pc, lr; BX lr`: a conditional return, which keeps its
fallthrough in the function (only returns do; other conditional PC writes are
mostly data decoded as ARM, and following them swamps the output).

## Functions that run into another function's prologue

m4a's `SoundMain` checks its lock, takes it (`ident++`), and runs straight on
into a `PUSH {r4-r7, lr}`. The prologue scan had made that `PUSH` a function
of its own, so SoundMain's C body ended after taking the lock and returned
holding it: the sound driver ignored every call from then on, and Advance
Wars' title screen, which waits on the music, fell back to the attract loop.
A block that falls through into another function's code now tail-calls it
when that code is a real prologue (`PUSH`/`STMDB sp!`) and the block didn't end
in a call. Other fall-throughs still return (see "Function entry and block
order"); `SWI 0` (SoftReset) is treated as never returning, which removes the
literal-pool fall-throughs behind the earlier boot breakage.

## BL to another function's BX

m4a calls through the `BX r3` that ends another function's
`POP {r3}; BX r3`, as a "call via r3". Seen alone, that BX looks like a POP+BX
return. The pattern only counts as a return when the POP is part of the
function being translated (`local_code_contains`).

## Shared blocks

Phase 6 used to give each block to exactly one function, "stealing" it from
any other. Two functions reaching one switch (Advance Wars' script
interpreter) then left one of them without its cases, which ran in the
interpreter. Blocks are shared now (each function emits its own copy), and
jump-table edges are followed like successors.

## The interpreter's return sentinel

The interpreter replaces LR with a sentinel to know when the routine it was
asked to run has returned. It never put the real LR back, so after a RAM
routine returned via `POP {r0}; BX r0` (or simply left LR alone), native code
carried on with `0xDEAD0001` in r0 or r14. It now maps the sentinel back to the
caller's LR in every register on exit.

## ARM block transfers

Whatever the addressing mode, LDM/STM put the lowest-numbered register at the
lowest address. The translator used to walk registers upward from Rn in every
mode, so `STMDB sp!, {r4-r7}` stored them reversed and the matching
`LDMIA sp!, {r4-r7}` handed the caller r4 and r7 swapped (and r5/r6). An LDM
that loads pc is a return.

## Stub functions

Code reachable only as the tail-call target of another function, with no
function of its own, is emitted from `translate_multi` as a "stub" by a
breadth-first walk from that address. The walk was capped at 32 blocks and
ignored jump-table edges, so larger stubs were silently truncated (the missing
blocks fell back to the interpreter) and their switches came out empty. It now
follows jump-table cases, with a cap of 4096 blocks as a runaway guard only.

## Compile time

MSVC's optimizer is superlinear in function size. One 14k-line function
(`func_0809E6D2`, under 200 blocks but ~5k instructions) held a single
translation unit at `/O1` for over an hour. Functions over 1500 instructions
(`HUGE_FUNC_INSNS`) are wrapped in `#pragma optimize("", off)`; there are about
20 in Advance Wars and they are mostly straight-line. The generated project
went from 35 minutes to under a minute.

## Dispatch depth

`cpu_bx` used to drop any indirect call nested more than 10 deep, as a guard
against the BX-as-return recursion above. With returns fixed, that only threw
away real callbacks; the limit is now 512 and it says when it fires.
