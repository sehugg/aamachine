# CLAUDE.md — 6502 engine and frontends

Guidance for working in `src/6502/`. The top-level `CLAUDE.md` covers the
repository as a whole; this file covers the assembler side.

## Ground rules

* Frontends implement the `io_*` routines; `engine.s` is included by a frontend
  and never assembled on its own.
* **Never use `:` inside a comment** — xa65 treats `:` as a statement separator
  even in comments, and the rest of the line is assembled.
* `.(` / `.)` are xa's local-label scopes. Labels inside are private, so the
  same `loop` / `done` / `fail` names recur everywhere.
* JS and 6502 engines must move in lock-step; `make -C test` diffs the two
  against the same gold files. Opcode constants live in `src/aavm.h`,
  `src/js/engine.js` and `engine.s` — a change touches all three.

## Targets in this directory

| Frontend | Platform | Notes |
|---|---|---|
| `c64_frontend.s` | Commodore 64 | crunched into `c64_crunched.bin`, 1541 drive code |
| `aambox_frontend.s` | synthetic test machine | driven by `aambox6502.c`, used by `make test` |
| `a2_frontend.s` | Apple II | ProDOS **or** ProRWTS2 storage, selected at assembly time |

## c64_frontend.s notes

* **REU (RAM Expansion Unit) as page cache.** Boot probes for a REU by
  writing a marker byte through its `$df01`-`$df0a` registers and reading it
  back with the size register wrapped through each of 8 bit positions
  (`gotreu`/`checkdone`, `c64_frontend.s:2629`-`2658`); zero page `reutop`
  (`$10`) ends up holding the detected size class, 0 meaning none. No REU
  disables undo outright ("No REU detected. Undo is disabled."). With a REU
  of at least 512K (`reutop >= 3`) the *entire* story is copied into REU at
  boot (`itxt_reufill`) and `io_readpage` is served straight from REU instead
  of disk — the whole page-cache/fault mechanism in `engine.s` effectively
  never faults to `io_getc`-speed disk I/O on a REU-equipped C64.
* **Wrong-disk detection.** `io_readpage` on a cache miss checks whether the
  disk currently in the drive is the story disk; if not, `iotxt_wrongdisk`
  (`c64_frontend.s:2187`) prints "Insert storydisk and press RETURN" and
  retries, rather than reading garbage. No dual-drive support (unlike the
  Apple II ProRWTS2 path) — there is exactly one drive to retry against.
* Progress-bar geometry: `PREXTRA=8`/`PRSHIFT=2` (bars are drawn from
  quarter-block PETSCII glyphs, hence the `<<2` scale), vs. `PRSHIFT=0` on
  the text-mode platforms (aambox, Apple II).
* Capacity is tight: as of this writing `gosling` (157,392-byte story) plus
  the crunched interpreter leaves only a few hundred bytes of slack on a
  169,984-byte `.d64` — treat the C64 target as close to its ceiling when
  adding engine or frontend code.

## engine.s: zero page and the io_* interface

`engine.s` is generic — no platform code, no undocumented opcodes, safe to run
from ROM. It owns zero page `$40`-`$ff` outright (register names are defined
at the top of the file, e.g. `specreg`/`inst`/`cont` at `$40`-`$4d`, `operlsb`/
`opermsb`/`result`/`rpair` at `$80`-`$8f` for arithmetic, `dictch`/`dicttbl`/
`dictpiv` etc. at `$90`-`$9e` for dictionary lookup, `stflag`/`screenw`/
`undosz`/`stybase`... at `$a0`-`$ad` for the status line, `codeseg`/`envbase`/
`heapsz`/`auxsz`/`ramsz` at `$c0`-`$cf`); a frontend must not touch that range
except through the documented interface. `zporg` at `$d0` (24 bytes) is a
small block of self-modifying dispatch code the engine writes into zero page
at init time. Heap layout is relative to a frontend-supplied `RAMEND`:
`HEAPEND = RAMEND-$300`, then `regs` (64 words), `inpbuf` (64 bytes),
`divstk` (8 words), the undo/chunk tables (`chnklsb`/`chnkssb`/`chnkmsb`),
`databuf`, `filesz`, and a physical-to-virtual page-cache map, all packed into
that fixed 768-byte tail below `RAMEND`.

A frontend must, before `#include "engine.s"`:

* Define `RAMEND` (top of engine RAM), and after the include, `SAFEPG`
  (`(*+$ff)>>8`, the first free page once the frontend's own `io_*` code and
  init routines are assembled) and `SAVEADDR` (where a savefile is staged —
  often `SAFEPG<<8` or, on Apple II, deliberately overlaid on `initsegment`
  since init code is dead once the engine starts). `SAVEMAXBYTES` (`$1000` by
  default) bounds how big a savefile/undo snapshot can be.
* Define the capability/geometry constants `DEFWIDTH` (screen columns),
  `PREXTRA`/`PRSHIFT` (progress-bar scaling: the bar's `y` total is
  `(width << PRSHIFT) - PREXTRA`), `HAVE_QUIT`, `HAVE_STATUS`, `HAVE_STYLE`,
  `UNDO`, `SAVERESTORE` — these `#if`-gate whole blocks of engine code, so a
  platform without save/restore storage (or without spare RAM for undo) can
  compile it out entirely rather than stub it.
* Implement every `io_*` routine the engine calls (see table below), then
  `#include "engine.s"`, then call `initengine0` through `initengine5` in
  order followed by `jmp startengine` (see `aambox_frontend.s` for the
  minimal example — `c64_frontend.s` and `a2_frontend.s` are the same shape).
  Each `initengineN` does one phase of boot: `0` sets `screenw`/`stflag`,
  `1` loads the first story page and computes RAM requirements, `2` skims the
  file's IFF-style chunk table, `3` sets up `LANG`/`DICT`, `4` primes `LOOK`,
  `5` prefetches some `CODE` pages; `startengine` saves the stack pointer to
  `savedsp` (used to unwind on a runtime error) and jumps into the fetch loop.

The `io_*` contract (canonical input/output comments live next to each label
in `aambox_frontend.s`, the simplest reference frontend):

| Routine | Contract |
|---|---|
| `io_mputc` | input a = char, to the main text window |
| `io_mclear` | clear the main window |
| `io_mline` / `io_mline_raw` | end the current line in the main window |
| `io_mflush` | flush any buffered output |
| `io_mstyle` | input a = style bits |
| `io_mprogress` | input x/y = progress/total, `0<=x<=y`, `y=(width<<PRSHIFT)-PREXTRA` |
| `io_sprepare` | input a = status-line height (`HAVE_STATUS` only) |
| `io_slocate` | input x = column, y = row |
| `io_sputc` | input a = char, to the status line |
| `io_sprogress` | same contract as `io_mprogress`, for the status line |
| `io_scommit` | status line is fully drawn |
| `io_getc` | output a = char (blocking key read) |
| `io_gets` | input ioparam = buffer (64 chars room); output y = length |
| `io_get_utf8` | output a = char, decoding multi-byte UTF-8 input; preserves y |
| `io_quit` | terminate (`HAVE_QUIT` only) |
| `io_random` | output x/y = lsb/msb, range `0..$7fff` |
| `io_restart` | reset engine state for a restart |
| `io_save` | input x/y = size lsb/msb, data at `SAVEADDR`; output c = success |
| `io_load` | verify the 12-byte `AASV` header; on success load to `SAVEADDR` and set c, else clear c |
| `io_undosupp` | output c = undo supported at runtime (RAM-dependent on some platforms) |
| `io_saveundo` | input x/y = byte size, ioparam = start of data; output c = success |
| `io_loadundo` | input x/y = byte size, ioparam = dest; output a = 0 ok / 1 no more entries |
| `io_readpage` | input x = physical RAM target page, ioparam = virtual address bits 23..8 (l-e); output a = physical RAM target page actually used (the page-cache hook) |

Frontends differ mainly in *how much* of this they can do cheaply: the C64
and Apple II both set `HAVE_STATUS=1`/`UNDO=1`/`SAVERESTORE=1`; `aambox6502`
(the automated-test target) sets `HAVE_STATUS=0`/`HAVE_STYLE=0` since the test
harness only diffs the main-window transcript. `HAVE_QUIT=1` only on aambox —
neither the C64 nor a `BASIC.SYSTEM`-less Apple II boot has anywhere to quit
*to*.

### The page cache and `io_readpage`

The page cache is a fixed-size ring of physical RAM pages living directly
below the heap, bounded by `firstpg`/`endpg` ($7e/$7f) and walked round-robin
by the cursor `evict` ($7d), which doubles as an "isn't set up yet" sentinel:
it reads `0` until `initengine1` has built the virtual-page table (`vtptr`/
`vtmsb`, $7a/$7c) and initializes it to `SAFEPG+1`. `fault` (the cache-miss
handler, `engine.s` near the `phydata`/`virdata` block) evicts the page at
`evict`, marks its old owner out-of-core in the virtual-page table, **installs
the new owner into `phy2lsb`/`phy2msb` before tail-calling `io_readpage`**,
and returns whatever `io_readpage` puts in `a` as the physical page actually
used. That ordering means `io_readpage` has no way to see which virtual page
it is replacing — by the time it runs, the old-owner tag is already gone — so
a frontend that wants write-back-on-evict (e.g. a "victim cache" that stashes
evicted pages in aux/expansion RAM instead of discarding them, noted as a
`a2_frontend.s` TODO) needs to capture the old owner earlier in the fault path
than `io_readpage` itself; the engine does not currently give it that hook.

Because the cache sits *below* the heap, and the heap grows downward as
`initengine1`-`4` allocate story structures, the cache window has to shrink
to stay out of the heap's way — `shrinkcache` (`engine.s`, called from
`allocwords` and from `initengine1`) clamps `endpg` down to `freeptr+1`
whenever `freeptr` has crossed into it, evicting anything now outside the new
window. Commit `d240bf6` ("6502 engine fix: page-cache window overlapped the
heap during init") is exactly this bug: before `shrinkcache` existed, a large
enough story (e.g. `dungeon`) could grow the heap into the cache's *hardcoded
initial guess* for its window, and page faults during the rest of init would
silently scribble story pages over just-loaded chunk data — it showed up as
garbled dictionary words leaking into game text. Any future rework of
init-phase memory layout that moves `freeptr` needs to keep calling
`shrinkcache` afterwards, or this class of corruption reappears.

## Building

```sh
make                    # everything
make a2.system          # Apple II only
```

`a2.system` is a two-stage build and the order matters:

1. `a2_frontend.bin` is assembled at `$2000` with `-DPRODOS=`/`-DPRORWTS=`.
2. `a2_prorwts2.bin` is assembled by **acme** (not xa) with
   `-DPRORWTS_ORG=$(size of a2_frontend.bin) + 8192`, so the ProRWTS2 blob
   lands exactly where the frontend's `prorwts2_init` symbol says it will.
3. `cat a2_frontend.bin a2_prorwts2.bin > a2.system`.

Because of step 2, **changing the size of the frontend changes where ProRWTS2
is assembled** — always rebuild both, never just one. `make clean` in this
directory removes both.

The storage backend is chosen by the `xa` flags in the `Makefile`
(`-DPRODOS=1 -DPRORWTS=0` or the reverse). Only one may be 1. Exactly one of
`SAVERESTORE`/`UNDO` blocks is compiled in as a result, and the banner text at
`txt_banner` reflects which.

Useful build artifacts: `a2.labels` (symbol → address, the fastest way to map a
trace address back to source) and `a2_frontend.lst` (full listing).

### Getting a change into a bundled story

`aambundle` embeds `a2.system` as `table_a2terp.h`, generated by `mkheader` at
`src/Makefile` time. A rebuilt `a2.system` does **not** reach
`example/*/AAM.SYSTEM` until `src/` is rebuilt too. When a change seems to have
no effect, check that first — compare `xxd -l 32` of `src/6502/a2.system`
against the bundled `AAM.SYSTEM`.

## Testing on the Apple II

izapple2 (https://github.com/ivanizag/izapple2, headless frontend) is a scriptable emulator.
It reads commands on stdin — `text` dumps the text screen as ANSI,
`type`/`key`/`enter` feed the keyboard, `png`/`pngm` grab screenshots, `trace`
tracers are selected with `-trace cpu|mos|mli|ss|ssreg`.

```sh
printf 'run 20000\ntext\nquit\n' | izheadless -model 2enh disk.po
```

Caveats learned the hard way:

* The emulator **starts running immediately at NTSC speed** and keeps running
  between commands, so results depend on how fast the commands arrive. `run N`
  (N thousand cycles) does not reliably gate, and `cycle` reports 0. For
  repeatable runs, drive it from a script that sleeps between commands rather
  than piping a fixed command list.
* `-trace cpu` produces millions of lines (≈4M for 12M cycles); redirect to the
  scratchpad and `grep -n` for addresses from `a2.labels`.
* `-model 2plus` exercises the 40-column, upper-case-folding path.
* `-speed full` runs the CPU flat out, which is much faster than waiting on
  NTSC timing when all you want to know is whether the thing booted.
* **`text` is not a reliable signal.** It sometimes renders a screen full of inverse `@`

Disk images:

```sh
python3 ../../mkprodos.py -o disk.po -s 140k a2.system,name=AAM.SYSTEM ../../example/cloak-rel2/STORY
python3 ../../mkprodos.py -o disk.po -s 140k --0boot a2_0boot.bin \
        a2.system,name=AAM.SYSTEM ../../example/cloak-rel2/STORY   # no ProDOS needed
ac -l disk.po          # list a ProDOS image
```

`mkprodos.py` and `bundle_apple2.c` build the same thing; the C
version is what ships. Both need a ProDOS release image to lift `PRODOS` and
the boot blocks from. `AAM.SYSTEM` must be the only `*.SYSTEM` file on the
volume, and the story volume must be named `AA.STORY`. Files too big for one
140k floppy are split into `AA.BOOT` + `AA.STORY`, with an 800k single-volume
image built as well.

The `DEBUG` build has a `verify` routine that Fletcher-8s every whole page read
through `io_readpage`; `fletcher8.py` computes the same checksum on the host,
and `todo.txt` has a table of reference values per story.

## a2_frontend.s memory layout

The full maps are in the header comment of `a2_frontend.s` — read them there.
The shape:

* ProDOS loads a SYS file at `$2000`. `boot_entry` copies a small mover to
  `$0300` and the mover relocates the image down to `$0800`, because leaving
  the interpreter at `$2000` costs 6 kB of page cache.
* There are **two** entry points, as a pair of jumps at `$2000`: ProDOS enters
  at `$2000`, 0boot at `$2003`. Which one ran is how the interpreter knows
  whether there is a ProDOS underneath it, so nothing has to guess from the
  contents of the global page. `raw_entry` plants `mlistub` and falls into
  `boot_entry`. `boothdrlen` covers the mover *and* the stub, both of which are
  assembled for elsewhere but live in the boot header.
* Code runs at `$0800`; the init segment above it is reclaimed as page cache
  once the engine starts, so anything called after `initengine5` must live
  below `SAFEPG`.
* `SAVEADDR = initsegment` — the savefile buffer deliberately overlays the init
  code.
* Page cache runs from `SAFEPG` up, heap from `RAMEND` down (`$bb00` under
  ProDOS, `$c000` under ProRWTS).
* On 128 kB machines aux `$0800-$bfff` is a page cache (via `AUXMOVE`) and aux
  `$d000-$ffff` is the undo ring. `AUXCACHETAG` at `$0300` holds one tag byte
  per slot; `auxslot` derives slot and tag by repeated subtraction so the cache
  size need not be a power of two.
* The undo ring is driven by `undomove`, which does its own `ALTZP` + language
  card banking and presents the two aux `$d000` banks as one flat 16 kB range.
  Between `SETALTZP` and `CLRALTZP` there is no zero page, no stack and no
  `jsr` — counters live on page 2, addresses ride in the copy loop.

### Language card conventions

`ROMCALL(addr)` / `ROMTAILCALL(addr)` abstract the difference:

* **ProDOS build** — ProDOS 8 owns the language card, system programs run with
  ROM banked in, so `ROMCALL` is a plain `jsr`.
* **ProRWTS build** — the resident state is *LC read bank 1*, which holds the
  relocated engine (`A2_ENGINE_HIMEM`, `himem_start`..`himem_end` at
  `$d000-$f400`). Bank 2 `$d000-$daff` holds ProRWTS2. So `ROMCALL` is
  `bit $c082 : jsr addr : bit $c088` — ROM in, call, bank 1 back. ProRWTS2
  calls are wrapped the other way, `lda $c083` twice then `lda $c088`.

Any code path that leaves the language card in a different state than it found
it will take the engine's `$d000` code out from under the next `jsr`.

## ProRWTS2

`a2_prorwts2.acme` is Peter Ferrie's driver (https://github.com/peterferrie/prorwts2),
vendored and configured in the option block at the top of the file. What the
current configuration gives us:

* `enable_write = 1` — but **the file must already exist, its size cannot
  change, and writes happen in whole 512-byte blocks**. Savefiles therefore
  have to be pre-created at a fixed size by the bundler, not created at
  runtime.
* `enable_seek = 1`, `aligned_read = 0` — `io_readpage` seeks by a byte delta
  from `next_rpage*` and rewinds (`rwts_rewind`, which zeroes `treeidx`,
  `blkidx`, `blkoff*`) when it needs to go backwards.
* `allow_subdir = 0` — no `/AA.STORY/STORY`; the file is opened by name in the
  current prefix.
* `allow_multi = 1` — bit 7 of `reqcmd` selects drive 2 in the current slot on
  the *open* call. This is the hook for two-drive operation.
* Its `init` calls the ProDOS MLI (`GET_PREFIX`, `READ_BLOCK`) and reads
  `DEVNUM` at `$bf30`, so **`prorwts2_init` must run before ProDOS is
  overwritten** — hence the `jsr prorwts2_init` at the very top of
  `boot_entry`, ahead of the mover. Anything that needs the MLI (locating the
  story volume, picking a drive) has to happen in that same window.
* ProRWTS2 uses zero page `$3b-$67` (`rwregs_first`..`rwregs_last`), which
  overlaps the engine's `$40+`. `swaprwregs` exchanges that range with
  `rwregsbuf` on page 2 around every call; `swapauxregs` does the same for
  `A1/A2/A4` around `AUXMOVE`.

## 0boot

`0boot.acme` is Peter Ferrie's one-shot track loader
(https://github.com/peterferrie/0boot), configured in the option block at the
top for our layout. It replaces ProDOS as the boot path on 5.25" images: it
loads `AAM.SYSTEM` straight off raw tracks and jumps into it, so the disk needs
no `PRODOS` file at all. That is 34 blocks (~17 kB) of story space back, a much
faster boot, and no dependency on finding a ProDOS release image to build a
disk. It is Disk II only — 800k and hard-disk images still want real ProDOS.

`a2_0boot.bin` is 498 bytes: a 256-byte boot sector assembled at `$800`, then
a 242-byte second stage assembled at `$900` but written to run in the zero
page. It is one-shot — after it jumps to the interpreter nothing of it is
needed, which is why using it this way avoids the relocation problem described
in `2026-08-11-131343-...txt`.

### What the boot PROM actually does

From the annotated `$C600` listing (Andy McFadden / 6502bench SourceGen). Two
facts drive the whole layout, and both are easy to get wrong:

* `$3d` is compared against the sector number **in the address field**
  (`c69a: cmp $3d`), i.e. the *physical* sector. The PROM knows nothing about
  `.dsk` or `.po` ordering.
* The PROM hands control to `$801` with `$3d` **already incremented to 1** and
  the buffer pointer at `$0900`:

  ```
  c6eb: e6 27   inc data_ptr+1     ; buffer -> $0900
  c6ed: e6 3d   inc sector         ; $3d = 1
  c6ef: a5 3d   lda sector
  c6f1: cd 00 08 cmp BOOT1         ; 1 >= 1, so we are done
  c6f8: 4c 01 08 jmp BOOT1+1
  ```

So 0boot's `inc $3d` asks for **physical sector 2**, not 1. ProDOS's own boot
block does the identical `inc $3d` / push `$Cs` / push `$5b` / `rts` dance, and
its second half lives at `.po` offset `$100` — which is how you can confirm
that physical sector 2 is ProDOS-logical sector 1. 0boot's `;the following
lives on sector $0E` comment is the same sector seen from a `.dsk`, where
physical 2 is DOS-logical `$E`.

### Sector ordering

ProDOS writes its logical sectors with a 2:1 interleave, logical to physical:

```
0,2,4,6,8,$a,$c,$e,1,3,5,7,9,$b,$d,$f
```

0boot's `sectbl` is indexed by *physical* sector and yields the position within
the track, so it wants the inverse of that, `0,8,1,9,2,$a,3,$b,4,$c,5,$d,6,$e,
7,$f`, with `interleave=1`. **Do not reach for `0,$e,$d,$c,...` here** — that
table converts between `.dsk` and `.po` orderings and is a different thing
entirely; using it silently scrambles every track after the first.

### Disk layout

* `.po` sector 0 — 0boot stage 1.
* `.po` sector 1 — 0boot stage 2 (physical sector 2), the same slot ProDOS
  uses.
* Blocks 2-6 — volume directory and bitmap, untouched, so the volume is still
  an ordinary ProDOS disk that AppleCommander or a hard-disk install can read.
* Block 7 — unused, the cost of alignment.
* Block 8 onwards — `AAM.SYSTEM`, contiguous, starting exactly on the track 1
  boundary because 0boot reads whole tracks.

`mkprodos.py --0boot` builds this. It allocates sapling and tree index blocks
*after* their data so that a file's payload starts at the first free block;
`bundle_apple2.c` still allocates them first, so the two no longer produce
byte-identical images and will need the same reordering when the 0boot path is
ported into the bundler.

### Faking the ProDOS global page

After `init`, ProRWTS2 has no ProDOS dependency at all — `DEVNUM` is read once
at `a2_prorwts2.acme:252` and the MLI is never called again on the floppy path.
`init` itself needs only:

* `$bf30` DEVNUM. 0boot supplies it from the slot already sitting in its
  patched PHASEOFF address (`$C0s0`, so `and #$70` is the drive-1 unit byte).
* One `GET_PREFIX` call. An **empty** prefix makes ProRWTS2 take its "no
  prefix" branch, which skips the directory walk, never issues `READ_BLOCK`,
  and lands on `bankram` before the SmartPort branch. So the 25-byte `mlistub`
  in `a2_frontend.s` — step the inline return address over three bytes, zero
  `$0200`, `clc` — is the entire emulation.
* `DEVADR01HI` (`$bf11 + 2*slot`) is only read and compared; a mismatch falls
  through to the Disk II slot-ROM signature check, which needs no ProDOS.

The stub is disposable: `RAMEND` is `$c000` in the PRORWTS build, so the heap
eats `$bf00` immediately afterwards.

### MACHID

There is no ProDOS to ask, so `setmachid` in `a2_frontend.s` works out the
three fields `detect` reads: machine class from the ROM ID at `$fbb3` (`$06`
means //e, //c or IIgs), the 80-column card from the Pascal 1.1 signature in
slot 3 (`$c305=$38`, `$c307=$18`, `$c30b=$01`, `$c30c=$8x` — the same test
ProDOS makes, so an accelerator in slot 3 is rejected), and aux RAM from
`auxtest`. It runs in **both** boot paths, so it is exercised on every boot
rather than only on 0boot disks. It is reached through `ROMCALL` because
`$fbb3` is under the language card.

Note that MACHID bits 5,4 are `01`=48K, `10`=64K, `11`=128K — testing
`and #$20` alone treats a 64K //e as having aux memory.

#### Why auxtest cannot use RAMRD

The obvious aux memory test — write a byte through RAMWRT, read it back
through RAMRD — **crashes**, and the reason is not subtle once seen: RAMRD
hands *every* read from `$0200` to `$bfff` to the auxiliary side, including the
opcode fetches of the routine doing the test, and `auxtest` lives at `$1fxx`.
The write half is harmless (RAMWRT does not affect fetches); the read half runs
whatever is in aux memory as code. Apple's own "Apple II Family
Identification" routine relocates its RAMRD test into zero page
(`safe = $0001`) for exactly this reason.

`auxtest` avoids relocation by using only switches that leave the fetch alone:

* **ALTZP** moves the zero page, the stack and `$d000-$ffff`. `auxtest` is in
  none of those — `ROMCALL` has left rom banked in over `$d000` — so between
  `SETALTZP` and `CLRALTZP` the rule is the same one `undomove` follows: no
  zero page addressing, no stack traffic, no `jsr`. `auxscratch` (`f_temp`)
  gets `$a5` on the aux side and `$5a` on the main side, and reading `$a5` back
  means the two sides are distinct memory.
* **80STORE + PAGE2** reaches aux `$0400-$07ff` on its own. This catches the
  case ALTZP alone cannot: a plain 1 kB 80-column card decodes none of the
  address lines above it and so answers at *every* aux address — the "sparse
  memory mapping" Apple's routine probes for. On such a card aux `$000b` and
  aux `$040b` are the same cell, so `auxtest` parks `$5a` in `auxmirror`
  (`$400 + auxscratch`, kept aliased by construction) and requires it to still
  be `$5a` after the ALTZP write. Setting 80STORE first and clearing PAGE2
  before clearing 80STORE keeps the display untouched, and the original byte is
  restored anyway.

ProDOS's `lda rdramrd / bmi muck128 / lda rdaltzp / bmi muck128` is not a
presence test — those switches exist on every //e whether or not the aux slot
is populated, and it only means "already banked in, so it must be there".
Nothing has touched either switch this early, so it would always fall through.

## Current state and open work

Tracked in `todo.txt` (which also holds raw traces and checksum tables). The
Apple II items in flight:

* **Save/restore under ProRWTS is implemented via a pre-created `SAVEFILE`.**
  ProRWTS2 can neither create a file nor grow one, so the bundler
  (`bundle_apple2.c`) writes an empty (all-zero) 4 kB `SAVEFILE` — size must
  match `SAVEMAXBYTES` in `engine.s` — next to `AAM.SYSTEM` on the boot
  volume, and `io_save` overwrites it in place. An all-zero file has no
  `AASV` header, which is how the interpreter tells "never saved" from a real
  savefile. `detect_wp` is now on in `a2_prorwts2.acme`'s option block, so a
  write-protected disk is caught instead of silently failing.
  When a story is too big for one 140k disk *with* a savefile alongside it but
  still fits *without* one, the bundler emits both: a single savefile-less
  disk (`DISKS_BOTH` in `bundle_apple2.c`) and a two-disk set that can save.
* **Two-disk ("dual drive") mode is implemented** (`c6f3d04`). `a2_frontend.s`
  tracks `sv_drive` (which drive a transfer should target, 0 = boot drive),
  `cur_drive` (which drive ProRWTS2 is currently open on) and `storydrive`
  (which drive `STORY` was actually found on, discovered once at
  `openstory` by retrying the open on the other drive and remembered for
  every later open). `rwts_openfile` sets bit 7 of `prorwts_reqcmd` — which
  ProRWTS2 reads as "swap to the other drive in the slot" rather than "select
  drive N" — only when `sv_drive` differs from `cur_drive`, so a single-drive
  machine (where both are always drive 0) never triggers a swap.
  `SAVEFILE` always lives on drive 0 (the boot disk) even when the story came
  off drive 1, because `savexfer` hardcodes `sv_drive = 0`.
* 80-column mode has possible cursor/newline bugs; `io_mstyle` is stubbed out
  because inverse mode misbehaves there (and it looks weird)
* **140k images ship in `.dsk` sector order, not `.po`.** `bundle_apple2.c`'s
  `write_image` reorders each track through the `dos_order` table (which is
  its own inverse) when writing a 140k disk, because most emulators and
  disk-writing tools expect DOS 3.3 sector order from a 5.25" image; 800k
  images have no such split and stay `.po`.
* RWTS18 is still only sketched in the `a2_frontend.s` header comment as a
  future storage backend.
* **Victim cache** (moving evicted page-cache pages to aux instead of
  discarding them) is noted as a TODO in `a2_frontend.s`'s header but not
  started. `engine.s` needed a modification to set phy2[lm]sb after calling
  io_readpage, not before.

## ProDOS zero page

From the ProDOS Technical Reference Manual, appendix A-4. Use by the Monitor
(M), Applesoft (A), Disk Drivers (D) and the ProDOS MLI (P):

```
  Hex---$0  $1  $2  $3  $4  $5  $6  $7  $8  $9  $A  $B  $C  $D  $E  $F
  0 $00 DA  DA  A   A   A   A                   A   A   A   A   A   A
 16 $10 A   A   A   A   A   A   A   A   A                           A
 32 $20 M   M   M   M   M   M   M   M   M   M   M   M   M   M   M   M
 48 $30 M   M   M   M   M   M   M   M   M   M   PMD PMD PMD PMD PMD DM
 64 $40 PMD PMD PMD PMD PMD PMD PMD PM  PM  PM  P   P   P   P   PM  M
 80 $50 MA  MA  MA  MA  MA  MA  A   A   A   A   A   A   A   A   A   A
 96 $60 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
112 $70 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
128 $80 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
144 $90 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
160 $A0 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
176 $B0 A   A   A   A   A   A   A   A   A   A   A   A   A   A   A   A
192 $C0 A   A   A   A   A   A   A   A   A   A   A   A   A   A
208 $D0 A   A   A   A   A   A           A   A   A   A   A   A   A   A
224 $E0 A   A   A       A   A   A   A   A   A   A
240 $F0 A   A   A   A   A   A   A   A   A   A
```

`$4e/$4f` are `RNDL`/`RNDH`, incremented by the Monitor's `RDKEY`/`KEYIN`.
They alias the engine's `rsim` register, which is why the Apple II frontend
delegates output to `COUT` but reads the keyboard itself.

## References

* https://prodos8.com/docs/techref/memory-use/
* https://pferrie.epizy.com/papers/porting.htm
* https://github.com/peterferrie/prorwts2
* https://github.com/peterferrie/0boot
* https://6502disassembly.com/a2-rom/ — annotated `$C600` Disk II boot PROM
* https://prodos8.com/docs/technote/misc/02/ — "Apple II Family
  Identification", the reference machine/memory detection routine
* https://savagetaylor.com/til/TA33130.html
