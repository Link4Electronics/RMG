# Project context

## Target platform

- **Architecture:** powerpc64 big-endian (PPC64 BE)
- **ABI:** ELFv2
- **OS:** Power Linux kernel 7.0.11
- **Page size:** 64 KB
- **Sub-arch:** Power Mac G4/G5 (AltiVec)
- **Dev machine:** x86_64 (has the repo cloned to make the changes)
- **Workflow:** edit files on dev machine → user compiles on G5 → user reports issues back

## Project overview

**RMG** (Rosalie's Mupen GUI, v0.8.9) — N64 emulator frontend combining:
- `RMG` — Qt6 GUI
- `RMG-Core` — C++ wrappers around mupen64plus-core C API
- `mupen64plus-core` — N64 emulator engine (R4300i MIPS III CPU)
- Video/audio/input/RSP plugins (GLideN64, paraLLEl-RDP, HLE RSP, etc.)

The target has **no assembly dynarec** — currently falls back to pure/cached interpreter. The upstream `mupen64plus-core` supports x86/x86_64/ARM/ARM64 dynarecs but **not PPC**.

## Big-endian state

`M64P_BIG_ENDIAN` is already handled in the core for:
- `osal/preproc.h` — byte swap offsets (`S8=0`, `S16=0`, `Sh16=0` on BE)
- `main/util.c` — savestate byte swapping (disabled on BE)
- `main/main.c` — ROM loading
- `pure_interp.c`, `cached_interp.c`, `cp1.c` — endian-aware memory paths
- `dbg_decoder_local.h` — debugger

Build system passes `-DM64P_BIG_ENDIAN` when `HOST_CPU` matches `ppc64*` / `powerpc64*`.

---

## PPC dynarec integration

### Reference: mupen64-360

**Source:** `https://github.com/gligli/mupen64-360`
**Origin:** Wii64 → mupen64-360 (Xbox 360 / Xenon PPC port)

### File inventory

All files in `device/r4300/ppc/`:

| File | Lines | Role |
|------|-------|------|
| `MIPS-to-PPC.c` | 2098+ | MIPS→PPC instruction translator (+ `emit_64bit_call`) |
| `PowerPC.h` | 1179 | PPC instruction encoding macros (~200 opcodes) |
| `ppc_dynarec.c` | ~1000 | Main entry: `dynarec()` loop, `decodeNInterpret()`, `dyna_mem()`, trampoline |
| `Recompile.h` | 318 | EMIT_* macros, block compilation interface |
| `MIPS.h` | 279 | MIPS opcode decoding helpers |
| `Recomp-Cache.c` | 256 | Recompiled code cache with LRU |
| `Recompile.c` | 382 | Block compilation, jump fixup, `genJumpPad()` |
| `Register-Cache.c` | 345 | GPR/FPR register allocator |
| `FuncTree.c` | 61 | BST for recompiled block lookup |
| `ppc_dynarec_compat.h` | 69 | Shim: extern globals, inline wrappers |
| `ppc_dynarec.h` | 15 | Public API declarations |
| `Register-Cache.h` | 30 | Register cache API |
| `Recomp-Cache.h` | 28 | Recomp cache API |
| `Wrappers.h` | 70 | DYNAREG bindings, function declarations, COP0 reg macros |
| `MIPS-to-PPC.h` | 29 | Translator API, `convert()` |

### Integration into r4300_core

- `r4300_core.c`: PPC_DYNAREC dispatch in `run_r4300()`, `invalidate_r4300_cached_code()`, `generic_jump_to()`
- `r4300_core.h`: Guards `struct recomp` with `#if !defined(NEW_DYNAREC) && !defined(PPC_DYNAREC)`; includes `ppc/ppc_dynarec.h`
- `cp0.c`: Guarded `dyna_jump()`/`recomp.dyna_interp` references with `#if !defined(NEW_DYNAREC) && !defined(PPC_DYNAREC)`

### Key adaptations from mupen64-360

| Feature | Original (mupen64-360) | RMG (PPC_DYNAREC) |
|---------|----------------------|-------------------|
| RDRAM base | `rdram->base` (global) | `r4300->rdram->dram` (per-instance) |
| Memory access | `read_word_in_memory()` macros | RMG's `mem_get_handler()`/`mem_read32()`/`mem_write32()` |
| Globals | Direct `reg[]`, `reg_cop0[]` arrays | Synced via `sync_r4300_state()`/`sync_back_state()` |
| Interpreter calls | `interp_ops[64]` table | Switch-based `decodeNInterpret()` |
| Heap | `__lwp_heap_*` (libogc) | `malloc()`/`free()` |
| Cache mgmt | Xenon `memdcbf`/`memicbi` | inline `dcbf`/`icbi` asm |
| FPU control | `mfspefscr`/`mtspefscr` (SPE) | Standard `mtfsf`/`mffs` (scalar) |
| Interrupt check | `check_interupt()` inline asm | C function calling `gen_interrupt()` |
| TLB | `tlb_map()` with bare args | `tlb_map(&r4300->cp0.tlb, entry)` |

### PPC64 type width fixes

PPC64 has 8-byte `long` and `unsigned long`. The recompiled PPC code uses 32-bit `lwz`/`stw` to access these globals, requiring exact 4-byte alignment:

| Global | Original type | Fixed type | Reason |
|--------|-------------|------------|--------|
| `reg_cop0[32]` | `unsigned long` | `uint32_t` | Accessed via `lwz` at offset `rd*4` |
| `FCR0`, `FCR31` | `long` | `uint32_t` | Accessed via `lwz` at r18+0 |
| `last_addr`, `interp_addr` | `unsigned long` | `uint32_t` | Accessed via `lwz` at r20+0 |
| `delay_slot` | `unsigned long` | `uint32_t` | Accessed via embedded address |

`reg[36]` stays `long long` (8 bytes) — correct, as recompiled code accesses low 32 bits at offset `i*8+4` on big-endian.

---

## Bug history

### Bug: `bl` ±32 MB range exceeded (FIXED)

**Symptom:** mmap'd code buffer can be 575+ MB from library text segment; all 14 CALL jumps silently overflow 24-bit LI field and jump to garbage.

**Fix:** Replaced all `EMIT_B(add_jump(..., 1, 1), 0, 1)` with `emit_64bit_call()` — loads full 64-bit address into r12, `mtctr` + `bctrl` for unlimited range. `MIPS-to-PPC.c`.

### Bug: `bl 4` trampoline wrong on Linux GAS (FIXED)

**Symptom:** Xenon assembler interprets `bl 4` as `bl .+4` (skip 1 instruction), but Linux GAS interprets it as `bl` to absolute address 4 — huge negative offset, skips `mtctr`, so `bctrl` hits uninitialized CTR (32-bit truncated address).

**Fix:** Changed `bl 4` to `bl .+4` (standard GAS idiom). Also merged two separate asm blocks (within the trampoline block) into one. `ppc_dynarec.c`.

### Bug: Xenon SPR encoding on G5 (FIXED — root cause of the crash)

**Symptom:** Crash at `0x00000000CC076770` (32-bit truncated address in CTR). The `bctrl` instruction jumps to garbage because CTR was never properly written.

**Root cause:** The `GEN_MTCTR`/`GEN_MFCTR` macros in `PowerPC.h` used `PPC_SET_SPR(ppc, 0x120)` for CTR and `PPC_SET_SPR(ppc, 0x100)` for LR. These values are **Xenon-specific SPR numbers** (SPRN=288 and SPRN=256 respectively). On standard POWER-PC (G5), SPR 288 is undefined, so `mtspr 288, r12` is either an illegal instruction or writes to an implementation-specific register. The real CTR (SPRN=9) was left uninitialized, containing garbage from a previous function call.

**Fix:** Changed SPR values from Xenon convention to standard POWER-PC numbers:
- `0x120` → `9` (CTR)
- `0x100` → `8` (LR)

The `PPC_SET_SPR` macro (`(spr & 0x3FF) << 11`) is **not usable** for `mtspr`/`mfspr` — it places the full 10-bit SPR at bits 20-11 (LSB0), but `mtspr` uses a **split encoding**: bits 25-21 = SPR[4:0], bits 20-16 = SPR[9:5], bits 15-11 = RS. See the next bug entry for the actual fix.

### Bug: FAILSAFE_REC_NO_VM not set (FIXED)

**Symptom:** Fast memory path (`genCallDynaMemVM`) emits direct PPC loads/stores at N64 KSEG1 addresses `(addr & 0x1FFFFFFF) | 0x40000000` — unmapped on Linux.

**Fix:** Set `failsafeRec |= FAILSAFE_REC_NO_VM` in `ppc_dynarec_init()` to force slow path via `dyna_mem()` C function. `ppc_dynarec.c`.

### Bug: Register allocator uses r2/r13 on PPC64 ELFv2 (FIXED)

**Symptom:** r2=TOC pointer, r13=thread pointer. If allocator maps a MIPS GPR to r2/r13, all C function calls from recompiled code have corrupted TOC/DSA.

**Fix:** Set `availableRegsDefault[2]=0` and `availableRegsDefault[13]=0` in `Register-Cache.c`. Only r24-r31 available for MIPS GPR mapping.

### Bug: D-cache/I-cache coherency (FIXED — REVERTED AND EXTENDED FOR PPC970)

**Symptom:** First compiled block hangs before any dyna_mem call. No debug output after `dyna_run` entry.

**Root cause:** PPC970 (G5) erratum — `icbi` does not invalidate I-cache when the D-cache line is still valid (clean but present). `dcbst` writes back but keeps the line valid; `dcbf` writes back AND invalidates, which is what `icbi` requires on PPC970.

**Original fix (for Xenon):** Changed `dcbf` → `dcbst` — correct on Xenon where `dcbf` is a hint, but WRONG on PPC970 where `dcbf` works and `dcbst` breaks icbi.

**First PPC970 fix:** Changed `dcbst` → `dcbf`. Also added `sync` before `isync` in ICInvalidateRange (kept). `Recomp-Cache.c:18`.

**Second PPC970 fix (Jun 14):** Changed separate `dcbf` loop + `icbi` loop to a **combined interleaved loop** (`dcbf` + `sync` + `icbi` per cache line). The hardware prefetcher can speculatively reload D-cache lines between two separate loops, re-validating the D-cache after `dcbf` but before `icbi`. This re-validation causes `icbi` to skip I-cache invalidation per the erratum. Even within a combined loop, `dcbf` is weakly ordered on PPC970 — `icbi` may execute before `dcbf`'s invalidation completes. Added `sync` between `dcbf` and `icbi` per cache line to force completion. `Recomp-Cache.c:13-30`.

### Bug: `get_physical_addr()` returned data instead of address (FIXED)

**Symptom:** TLB translation returned loaded data values instead of translated addresses.

**Fix:** Added proper TLB address translation path. `ppc_dynarec.c`.

### Bug: `genJumpPad()` LR not restored (FIXED)

**Symptom:** `genJumpPad()` emitted `BLR` without restoring LR.

**Fix:** Added `LD r0, DYNAOFF_LR(r1)` + `MTLR r0` before `BLR`. `Recompile.c`.

### Bug: `lwz` for LR restore on PPC64 (FIXED)

**Symptom:** `lwz` used for LR restore (32-bit on PPC64) — only restores low 32 bits.

**Fix:** Changed to `ld` (64-bit). `MIPS-to-PPC.c`.

### Bug (avoided): `genUpdateCount()` CMPL operand order (INTENTIONALLY LEFT AS-IS)

**Analysis:** The Count-vs-next_interrupt comparison uses `CMPL(tmp, 0, 2)` at `MIPS-to-PPC.c:1918`:
- `GEN_CMPL(ppc, ra, rb, cr)` emits `cmpl cr, 0, ra, rb`
- `EMIT_CMPL(tmp, 0, 2)` = `cmpl cr2, 0, rTmp, r0` — compares **next_interrupt** with **Count**
- `BLELR(2,0)` at `MIPS-to-PPC.c:217` (BO=4, BI=CR2.GT) branches to LR when GT=0
- So BLELR fires (returns to dispatcher) when `next_interrupt <= Count` — i.e., **exactly when an interrupt is due**

Swapping to `EMIT_CMPL(0, tmp, 2)` would invert the sense: BLELR would fire when `Count <= next_interrupt` (returning too early, before any interrupt), and fall through when `Count > next_interrupt` (looping forever in backward jumps). **The original ordering is correct; keep `(tmp, 0, 2)`**.

### Bug: Fallthrough address ternary miscomputed for non-NOP delay slots (FIXED)

**Symptom:** The NOT-taken fallthrough path in `branch()` computed the re-entry address as `get_src_pc() + (delaySlot ? 8 : 4)`. After `check_delaySlot()`, `get_src_pc()` returns the delay slot address (`pc+4`). For NOP delay slots this gave `pc+8` (correct), but for non-NOP delay slots it gave `pc+12` (off by 4). This caused the dispatcher to skip the first instruction after the delay slot on re-entry.

**Root cause:** Xenon-ism where `get_src_pc()` had different semantics (returned `src_pc_val` instead of `src_pc_val - 4`). The `delaySlot` ternary was correct for Xenon but wrong for RMG.

**Fix:** Changed `get_src_pc() + (delaySlot ? 8 : 4)` to `get_src_pc() + 4` — which is always correct because `get_src_pc()` already accounts for the consumed delay slot. `MIPS-to-PPC.c:209-210`.

### Bug: MTFSFI immediate field at wrong bit position (FIXED)

**Symptom:** `GEN_MTFSFI` used `PPC_SET_IMMED` (shift=0, bits 0-15) for the immediate field, but `mtfsfi` in XFL-form places the U immediate at integer bits 16-22. When `set_rounding(PPC_ROUNDING_CEIL)` or `set_rounding(PPC_ROUNDING_FLOOR)` is called (e.g., by MIPS `ceil.w.s/d` or `floor.w.s/d`), the immediate value 2 or 3 corrupts the extended opcode field (bits 1-10), turning `mtfsfi` into a completely different instruction. TRUNC (value 0, using `FCTIWZ` bypass) and NEAREST (value 0) were unaffected.

**Fix:** Replaced `PPC_SET_IMMED(ppc, (immed))` with `ppc |= ((immed) & 0xF) << 16` for correct placement at integer bits 16-19 (7-bit U field, lower 4 bits used). `PowerPC.h:1026`.

### Bug: HEAP_PARENT shift wrong (FIXED)

**Symptom:** `HEAP_PARENT(i) = ((i-1)>>2)` uses divide-by-4 instead of divide-by-2 (`(i-1)>>1`). In a binary min-heap, the parent of node i should be `(i-1)/2`. This meant nodes at indices >= 3 computed incorrect parent indices, breaking the LRU eviction ordering in the recompiled code cache. Hot code could be prematurely evicted.

**Fix:** Changed `>>2` to `>>1`. `Recomp-Cache.c:46`.

### Bug: Backward branch BLR safety net (FIXED)

**Symptom:** Backward conditional branches could loop within the block without ever returning to the dispatcher if the Count check somehow fails (e.g., stale `next_interupt` global).

**Fix:** At `MIPS-to-PPC.c:219-222`, for `offset < 0`: emit `LD r0, DYNAOFF_LR(1); MTLR r0; BLR` after the BC(nbo) branch. This guarantees every taken backward branch returns to the dispatcher, preventing infinite internal loops.

### Bug: `dyna_run()` split asm blocks cause GCC register corruption (FIXED)

**Symptom:** First compiled block at `0xA4000040` executes and never returns — `dyna_run()` bctrl never comes back even though the compiled code has a BLR at the end.

**Root cause:** Two separate `__asm__ volatile()` blocks in `dyna_run()`:
1. First block sets up r14-r23 (declared clobbered → dead to GCC)
2. Second block calls compiled code via `bctrl`, then captures outputs

With optimization ≥ `-O1`, GCC's register allocator reuses r14-r23 (declared dead by first block) for the second block's input operands. When `code` gets assigned to r22, the compiled code (via `bctrl`) reads a corrupted `func` pointer — `EMIT_STW(3, 0, REG_LOCALRS)` writes to `code+0` instead of `func+0`, corrupting the code buffer or memory-mapped I/O, causing unpredictable behavior including infinite loops.

**Fix:** Merged both asm blocks into one so register setup happens in the same block as the bctrl call. Operand renumbered: inputs %4-%13 (9 setup ptrs + code), outputs %0-%3 (naddr, link_branch, return_addr, last_func). `ppc_dynarec.c:98-137`.

### Bug: Canary trampoline overwrites DYNAREG_ZERO (r23) (FIXED)

**Symptom:** First compiled block at `0xA4000040` executes and never returns — `dyna_run()` bctrl never comes back even though the compiled code has a BLR at the end. The `dyna_mem()` C function is never reached (no `[dyna_mem]` debug output).

**Root cause:** The canary trampoline in `dyna_run()` loads `r23 = dyna_canary` (`ld 23, 88(%0)` at line 134). But `DYNAREG_ZERO = 23` (`Wrappers.h:15`). Every time the compiled code references MIPS register `$zero` via `mapRegister(0) → DYNAREG_ZERO → r23`, it gets the canary address instead of 0. This corrupts all ALU operations, stores, and memory accesses using `$zero`, including the llbit and COP0 register stores at `MIPS-to-PPC.c:1142,1232` which use `EMIT_STW(DYNAREG_ZERO, ...)`.

**Fix:** Moved the canary pointer from r23 to r31:
- `ppc_dynarec.c` trampoline: `ld 23, 88(%0)` → `ld 31, 88(%0)` + `li 23, 0` (restore DYNAREG_ZERO), `stw 0, 48(23)` → `stw 0, 48(31)`
- `MIPS-to-PPC.c` genCallDynaMem: all `EMIT_STW(0, offset, 23)` → `EMIT_STW(0, offset, 31)`
- `Register-Cache.c`: `availableRegsDefault[31] = 0` (removed r31 from allocator pool)

### Bug list

| Bug | File | Lines | Status |
|-----|------|-------|--------|
| `bl` ±32 MB range exceeded | `MIPS-to-PPC.c` | emit_64bit_call() | FIXED |
| `bl 4` GAS vs Xenon | `ppc_dynarec.c` | 97-136 | FIXED |
| Xenon SPR encoding (CTR=288, LR=256) | `PowerPC.h` | 394, 401, 842, 849 | FIXED |
| FAILSAFE_REC_NO_VM not set | `ppc_dynarec.c` | 488 | FIXED |
| r2/r13 register corruption | `Register-Cache.c` | 13-17 | FIXED |
| D/I-cache coherency (dcbst vs dcbf + combined loop) | `Recomp-Cache.c` | 13-30 | FIXED |
| `get_physical_addr()` data vs address | `ppc_dynarec.c` | 533-544 | FIXED |
| `genJumpPad()` LR not restored | `Recompile.c` | 173-174 | FIXED |
| `lwz` for LR restore on PPC64 | `MIPS-to-PPC.c` | 1095, 1912 | FIXED |
| Backward branch BLR safety net | `MIPS-to-PPC.c` | 219-222 | FIXED |
| Fallthrough address ternary | `MIPS-to-PPC.c` | 209-210 | FIXED |
| Stale `next_interupt` after interrupt | `ppc_dynarec.c` | 246-247 | FIXED |
| `decodeNInterpret` sentinel | `ppc_dynarec.c` | 732 | FIXED |
| GEN_BNE/GEN_BNELR BO encoding | `PowerPC.h` | 565, 1125 | FIXED |
| MTFSFI immediate field position | `PowerPC.h` | 1021-1026 | FIXED |
| HEAP_PARENT shift (>>2→>>1) | `Recomp-Cache.c` | 46 | FIXED |
| Split asm blocks in dyna_run | `ppc_dynarec.c` | 98-137 | FIXED |
| branch() BO/BI values (CR4 + polarity) | `MIPS-to-PPC.c` | 197-205 | FIXED |
| start_new_block() stale-code | `MIPS-to-PPC.c` | 112-116 | FIXED |
| rldicl after lis | `PowerPC.h`, `Recompile.h` | 627-636, 165-166 | FIXED |
| branch() bo/nbo inversion (all six conditions) | `MIPS-to-PPC.c` | 197-205 | FIXED |
| r23/DYNAREG_ZERO overwritten by canary trampoline | `ppc_dynarec.c`, `MIPS-to-PPC.c`, `Register-Cache.c` | 134-136, 1974-1990, 18 | FIXED |
| Canary-based emit_64bit_call ignores target | `MIPS-to-PPC.c` | 54-65 | FIXED |
| GEN_RLDICL sh[5] at wrong bit (sub-opcode corruption) | `PowerPC.h` | 627-636 | FIXED |
| GEN_RLDICR sub-opcode bit missing for sh<32 | `PowerPC.h` | 615-625 | FIXED |
| emit_64bit_call stw+ld+sync to avoid 64-bit rotates | `MIPS-to-PPC.c` | 63-68 | FIXED |
| GEN_SYNC/EMIT_SYNC macros added | `PowerPC.h`, `Recompile.h` | 382-385, 101-102 | FIXED |
| dyna_canary 8-byte alignment for ld | `ppc_dynarec.c` | 49 | FIXED |
| Asm clobber list: `%fr14`→`fr14` (GCC silently ignored) | `ppc_dynarec.c` | 178 | FIXED |
| Asm clobber list: missing r0 (used by trampoline) | `ppc_dynarec.c` | 173-178 | FIXED |
| dyna_mem canary slot 1 conflicts with emit_64bit_call stw | `ppc_dynarec.c` | 878 | FIXED — moved to slot 9 (C-code-only) |
| Over-aggressive asm clobbers (`r3-r7,cr1,fr0-fr13`) cause GCC `impossible constraints` | `ppc_dynarec.c` | 174-180 | FIXED — removed, keep only r0 and r14-r31/fr14-fr27 |
| GEN_RLDICR bits 28-29 missing (emitted unknown instruction) | `PowerPC.h` | 619-632 | FIXED |
| GEN_RLDICL bits 28-29 missing (emitted unknown instruction) | `PowerPC.h` | 634-646 | FIXED |
| `emit_64bit_call` sldi+or wrong on PPC970 (switched to stw+ld+sync fallback + ld-from-canary) | `MIPS-to-PPC.c` | 38-88 | FIXED |
| `dyna_canary` expanded 16→48 elements | `ppc_dynarec.c` | 49 | ACTIVE |
| emit_64bit_call stw low/high order reversed on BE (fallback path) | `MIPS-to-PPC.c` | 61-74 | FIXED |
| Direct C call test of dyna_test before asm trampoline | `ppc_dynarec.c` | 147-158 | ACTIVE |
| Pre-store function addresses in canary for ld-from-canary approach | `ppc_dynarec.c`, `MIPS-to-PPC.c` | 317-330, 46-88 | ACTIVE |
| mfctr reads 0 after mtctr (SPR rename stall on PPC970) | `MIPS-to-PPC.c` | 81-83 | FIXED |
| GEN_ISYNC/EMIT_ISYNC macros missing | `PowerPC.h`, `Recompile.h` | 382-385, 101-102 | FIXED |
| X-form shift/ALU source/dest swapped (SRAWI, SLW, SRW, SRAW, AND, NAND, ANDC, NOR, OR, XOR) | `PowerPC.h` | 447-530 | FIXED |
| SPR macros (MTCTR, MFCTR, MTLR, MFLR) wrong split encoding — SPR field overlapped register field | `PowerPC.h` | 394, 401, 842, 849 | FIXED |
| SPR macros: secondary fix — RS field at <<21, SPR[4:0] at <<16, SPR[9:5] at <<11 | `PowerPC.h` | 402-420, 878-892 | FIXED |
| GEN_ISYNC opcode wrong: used PPC_OPCODE_X (31) instead of PPC_OPCODE_XL (19) — emitted stwcx. r0,r0,r0 instead of isync | `PowerPC.h` | 389 | FIXED |
| GEN_MFCTR/GEN_MFLR: RT and SPR[4:0] fields swapped — mfctr read from SPR 22 instead of SPR 9, mflr read from SPR 21 instead of SPR 8 | `PowerPC.h` | 411-417, 881-887 | FIXED |
| D/I-cache separate-loop gap (PPC970 prefetcher re-validates D-cache between dcbf and icbi) | `Recomp-Cache.c` | 13-30 | FIXED — combined interleaved loop (note: SIGILL was actually SPR macro bug, not cache coherency) |
| Missing sync before dcbf loop — stores still in store buffer when dcbf runs | `Recomp-Cache.c` | 23 | FIXED |
| genCallDynaMem/ genCheckFP bypass (store args + BLR return instead of bctrl) | `MIPS-to-PPC.c`, `ppc_dynarec.c` | genCallDynaMem, genCheckFP, 361-386 | FIXED |
| `code_addr[]` not populated for non-jump-target instructions (OOT infinite recompilation) | `MIPS-to-PPC.c` | convert() | FIXED — added `reset_code_addr()` at start of `convert()` |
| genCallInterp uses `bctrl` from mmap'd buffer (SM64 crash after decodeNInterpret) | `MIPS-to-PPC.c`, `ppc_dynarec.c` | genCallInterp, 361-386 | FIXED — converted to bypass approach + dispatcher case 3 |
| BUTTONS struct bitfields reversed on BE (MSB-first packing) | `m64p_plugin.h` (4 copies), `input_plugin_compat.c` | 150-174, 37-41 | FIXED |
| VRU direct `Keys->Value = 0x0020` wrong on BE | `RMG-Input/main.cpp` | 1338 | DEFERRED (VRU not used) |
| Periodic return from pure-ALU compiled code (PPCD_CHECK_INTERVAL clamping) | `ppc_dynarec.c` | 51-60, 347-367 | FIXED — clamp next_interrupt to Count+10000 before each dyna_run, restore after |
| KSEG0/KSEG1 not stripped in read_rmg_word/write_rmg_word — all I/O register writes go to open_bus (PI DMA never starts, IPL3 spins forever at 0xA4000428) | `ppc_dynarec.c` | 990-1001 | FIXED — added `vaddr &= 0x1ffffffc` when `(vaddr & 0xC0000000) == 0x80000000`, matching r4300_read_aligned_word |
| Double-buffer flicker — `SCREEN_UPDATE_AT_VI_UPDATE` swaps without render, clearing buffer between frames | `Video.cpp` | 207-216 | FIXED Jun 18 — gated swap with `status.bScreenIsDrawn`, matching `SCREEN_UPDATE_AT_VI_UPDATE_AND_DRAWN` behavior |

### Known issues

1. **Floating-point control** — `fesetround()` / `mtfsf` / `mffs` path needs runtime verification that rounding mode is set correctly for N64 FE_TOWARDZERO emulation.
2. **No VMX128 in the CPU dynarec** — The recompiler emits only scalar PPC (add, lwz, stw, rlwinm, etc.). LVX/STVX/VOR macros are standard AltiVec and work on both G4/G5 and Xenon.
3. **bctrl never reaches function body (Jun 14)** — Three PowerPC.h macro bugs fixed Jun 13 (X-form swap, SPR split encoding, GEN_ISYNC opcode). After rebuild, bctrl still never returns from mmap'd code buffer. **Approach changed**: genCallDynaMem/genCheckFP now bypass bctrl entirely, storing args to canary slots + BLR return to C dispatcher. See "Current focus" for details.
4. **GEN_RLDICR/GEN_RLDICL bits 28-29 missing** — These macros emitted unknown instructions on PPC970 by leaving bits 28-29 at 00 instead of 11/10. Fixed Jun 10. EMIT_SLDI (used by sldi+or approach) was affected, but ld-from-canary approach bypasses it entirely.

### Bug: Asm clobber list uses `%fr14` instead of `fr14` (FIXED)

**Symptom:** GCC silently ignores `"%fr14"` through `"%fr27"` in clobber list (invalid register name format). If the compiled code or `dyna_mem()` uses those FP registers, the caller's FP state is silently corrupted.

**Root cause:** GCC inline asm clobber names are bare register names (`"fr14"`), not `%`-prefixed operand references (`"%fr14"`). The `%` prefix is only valid in asm template strings for operand substitution. In clobber lists it's treated as an unknown register and silently ignored.

**Fix:** Changed all 14 `"%frNN"` to `"frNN"`. Also added missing r0 (used by trampoline for `li`/`stw`). `ppc_dynarec.c:173-178`. Note: did NOT add r3-r7/cr1/fr0-fr13 — those are volatile-by-ABI and GCC handles them implicitly, and including them would starve GCC of registers for asm operands.

### Bug: dyna_mem canary slot 1 conflicts with emit_64bit_call stw (FIXED)

**Symptom:** `dyna_canary[1] = 0xDEAD` in `dyna_mem()` entry was always overwritten by `emit_64bit_call`'s `EMIT_STW(12, 4, 31)` before dyna_mem could run. Made it look like dyna_mem was never entered.

**Fix:** Moved dyna_mem's entry marker to canary[9] (C-code-only slot, not written by compiled code). `dyna_mem()` now sets `dyna_canary[9] = 0xDE`. Slot 9 shows `0xAA` before asm, `0xDE` if dyna_mem entered, `0xFF` after asm returns. `ppc_dynarec.c:878`.

### Bug: X-form shift/ALU source and destination swapped in 10 PowerPC.h macros (FIXED)

**Symptom:** SIGSEGV at `isync` instruction (PC=0x50 in compiled code buffer). Deferred DSI (trap=0x300) with `si_addr=0xBB`. The crash instruction itself (`isync`) was correct — the fault was from a prior instruction whose corrupt value was only consumed (loaded into an address register) much later.

**Root cause:** Ten X-form macros in `PowerPC.h` had RS (source, bits 6-10) and RA (destination, bits 11-15) swapped. In X-form, the operand encoding is:
- Bits 6-10: RS = source register
- Bits 11-15: RA = destination register

But the macros used `PPC_SET_RD` (bits 6-10, XO-form dest convention) for the *destination* parameter and `PPC_SET_RA` (bits 11-15) for the *source* parameter — the opposite of what X-form requires. Every call that passed `(dest, src, ...)` emitted `op src, dest, ...`.

Concrete example: `_flushRegister()` in `Register-Cache.c:26` calls `EMIT_SRAWI(0, rLO, 31)` intending `srawi r0, rLO, 31` (sign-extend LO value into r0). The old macro emitted `srawi rLO, r0, 31` — sign-extending garbage in r0 into rLO, then storing rLO as the HI word. This corrupted the register value, which propagated through downstream instructions until an `addi` computed an unmapped address from the garbage, triggering a DSI that was deferred to the next context-synchronizing instruction (`isync`).

**Affected macros:**
- Shifts: `GEN_SRAWI`, `GEN_SLW`, `GEN_SRW`, `GEN_SRAW`
- ALU: `GEN_AND`, `GEN_NAND`, `GEN_ANDC`, `GEN_NOR`, `GEN_OR`, `GEN_XOR`

**Fix:** Swapped `PPC_SET_RD` ↔ `PPC_SET_RA` in all 10 macros so callers can naturally write `(dest, src, ...)`. `PowerPC.h:447-530`.

**Note:** `GEN_EXTSW` (lines 493-498) has the same bug but is never called, left unfixed.

### Bug: SPR macros (MTCTR, MFCTR, MTLR, MFLR) wrong split encoding — SPR field overlapped register field (FIXED)

**Symptom:** `mtctr r12` was emitted as `mtspr 288, r12` — a Xenon-specific SPR (SPRN=288) instead of the standard CTR (SPRN=9). On PPC970, `mtspr 288` is either illegal or writes to an implementation-specific register, leaving CTR uninitialized (containing garbage from a previous function call). When `bctrl` later read CTR via the branch unit, it jumped to garbage, crashing or hanging.

**Root cause:** `PPC_SET_SPR(instr, spr)` puts the full 10-bit SPR number at bits 20-11 (LSB0): `instr |= (spr & 0x3FF) << 11`. But `mtspr`/`mfspr` use a **split encoding** per the ISA (LSB0 numbering):
- Bits 25-21 (<<21): SPR[4:0] (low 5 bits of SPR number)
- Bits 20-16 (<<16): SPR[9:5] (high 5 bits of SPR number)
- Bits 15-11 (<<11): RS (for `mtspr`) or RT (for `mfspr`)

With `spr=9` (CTR), `PPC_SET_SPR` placed 9 at bits 20-11:
- Bits 20-16 = 0b00000 (SPR[9:5]) — correct by coincidence (9>>5=0)
- Bits 15-11 = 0b01001 (RS field!) — **WRONG**, should be r12, gets SPR[4:0]=9 instead

And the RS field at <<21 (from `PPC_SET_RD` in the original code):
- Bits 25-21 = 0b01100=12 (SPR[4:0] field!) — **WRONG**, should be r12 in RS, gets RS=12 in SPR[4:0]

This encoded SPR=(0<<5)|12=12 → `mtspr 12, r9` (writes r9 to SPR 12) instead of `mtctr r12`.

For **mfspr**, the field layout is the same but RT replaces RS. The original `GEN_MFCTR`/`GEN_MFLR` used `PPC_SET_RD` (<<21) for RT — also **wrong** for the same reason.

**Fix:** Replaced `PPC_SET_SPR(ppc, spr)` + `PPC_SET_RD` with manual split encoding for each of the four macros (LSB0):
```
ppc |= ((spr) & 0x1F) << 21;       // SPR[4:0] at bits 25-21
ppc |= (((spr) >> 5) & 0x1F) << 16; // SPR[9:5] at bits 20-16
PPC_SET_RB(ppc, reg);              // RS/RT at bits 15-11
```

`PowerPC.h:394,401,842,849`. Example: for `mtctr r12`, the manual split produces `0x7D2063A6` — correct per ISA: SPR[4:0]=9 at <<21, SPR[9:5]=0 at <<16, RS=12 at <<11.

---

## GLideN64 endian issues (PPC64 BE)

### RDRAM byte order

mupen64plus-core stores RDRAM in **host byte order**:
- x86 LE: N64 BE data written to RDRAM is stored as LE words (byte-reversed within each 32-bit word)
- PPC64 BE: N64 BE data written to RDRAM is stored as BE words (native byte order)

`*(u32*)&RDRAM[addr]` always produces the correct N64 value regardless of host endianness — the LE pointer dereference reverses byte order in the same way the LE storage does, and the BE pointer dereference is a no-op. Both produce the same u32.

However, `*(s16*)&RDRAM[addr]` and byte-level accesses differ:
- On LE: `*(s16*)&RDRAM[0]` reads bytes [byte0, byte1] as a LE s16. Since the 32-bit word containing the s16 is stored in LE, the two s16 halves of a word are REVERSED (low s16 at even index, high s16 at odd index).
- On BE: `*(s16*)&RDRAM[0]` reads bytes as a BE s16. The s16 halves are in correct N64 order (high at even index, low at odd index).

### LE-specific XOR patterns in GLideN64

GLideN64 was designed for x86 LE and uses ad-hoc XOR patterns on pointers/indexes to compensate for the LE byte-reversed storage of 16-bit and 8-bit values within 32-bit words. These patterns MUST be compiled out (or made NOP) on PPC64 BE.

| Pattern | Purpose on LE | Problem on BE |
|---------|---------------|---------------|
| `((short*)RDRAM)[i ^ 1]` | Swap adjacent s16 halves within a 32-bit word | Reads WRONG s16 (different word half) |
| `*(s16*)&RDRAM[addr ^ 2]` | Swap bytes of a s16 at misaligned byte address | Reads bytes from wrong offset |
| `RDRAM[(base + N) ^ 3]` | Reverse byte within 32-bit word for u8 access | Reads byte from wrong position |
| `((u16*)RDRAM)[i ^ 1]` | Same as short pattern for u16 | Reads wrong u16 |
| `GRAPHICS_ADDRESS_LOAD` etc. (custom macros) | Byte-swap packed data | Corrupts on BE |

### Affected files (comprehensive)

All patches MUST use `__BIG_ENDIAN__` / `__BYTE_ORDER__` detection (NOT `M64P_BIG_ENDIAN`, which is not passed to GLideN64's build).

| File | Pattern | Data affected |
|------|---------|---------------|
| `gSP.cpp:344-351` | `((short*)RDRAM)[(addrShort+4)^1]` | Light position s16 |
| `gSP.cpp:388-391` | `((short*)RDRAM)[(addrShort+16)^1]` | Light position s16 (CBFD) |
| `gSP.cpp:410-415` | `((s16*)RDRAM)[(addrShort+0)^1]` / `((u16*)RDRAM)[(addrShort+6)^1]` | Light pos s16, la/qa u16 (Acclaim) |
| `gSP.cpp:347-349, 416-418` | `RDRAM[(addrByte + N) ^ 3]` | Light ca/la/qa u8 |
| `gSP.cpp:1088-1095` | `*(s16*)&RDRAM[address ^ 2]` / `*(u8*)&RDRAM[(addr + N) ^ 3]` | DMA vertex s16 xyz + u8 rgba |
| `gSP.cpp:265-272` | `*(s16*)&RDRAM[address + N]` (no XOR) | Viewport vscale/vtrans — ALREADY CORRECT on BE |
| `RSP_LoadMatrix.cpp:10-14` | `_N64Matrix *m = (_N64Matrix*)&RDRAM[...]` + `[j ^ 1]` | Matrix s16 int + u16 frac |
| `GraphicsDrawer.cpp:1242-1244` | `(u16*)(RDRAM + ...)` + `[i ^ 1]` | Depth pixel writes |
| `GraphicsDrawer.cpp:1323-1325` | `(u16*)(RDRAM + ...)` + `[i ^ 1]` | Palette writes |
| `ZSortBOSS.cpp:181-188` | `((s16*)RDRAM)[(a+N)^1]` | Viewport/fog s16 |
| `ZSortBOSS.cpp:691` | `((s16*)RDRAM)[((addr+(i<<4)+(j<<1))>>1)^1]` | Matrix/table s16 |
| `ZSort.cpp:466-473` | `*(s16*)&RDRAM[(a+N)^1]` / `((s16*)RDRAM)[(a+N)^1]` | Viewport s16 |
| `F5Indi_Naboo.cpp:490` | `CAST_RDRAM(const u16*, ...)[(shift+N)^1]` | Vertex u16 |
| `BufferCopy/WriteToRDRAM.h:45-57` | `_dst[numStored ^ _xor]` with `_xor=1` | 16-bit pixel writes |

### Struct casts from RDRAM (no byte-level XOR)

These multi-byte struct fields are read directly from RDRAM via `(T*)&RDRAM[addr]`. On BE the byte order is correct (both host and N64 are BE). On LE the byte order within each multi-byte field is reversed by the pointer dereference, which is the intended behavior.

However, any struct that is read via a cast AND then accessed with XOR patterns internally (e.g. `_N64Matrix`) needs both the struct definition AND the XOR access to be endian-aware.

| Struct | File:Line | Multi-byte fields |
|--------|-----------|-------------------|
| `Vertex` (s16 x,y,z; s16 s,t; s8 norm; u8 rgba) | `gSP.cpp:1005` | All s16 |
| `PDVertex` (s16 x,y,z; s16 s,t; u32 ci) | `gSP.cpp:1073` | s16 + u32 |
| `SWVertex` (s16 y,x; u16 flag; s16 z) | `gSP.cpp:1292+` | All s16/u16 |
| `T3DUXVertex` (s16 y,x; u16 flag; s16 z) | `gSP.cpp:1324` | All s16/u16 |
| `_N64Matrix` (s16 int[4][4]; u16 frac[4][4]) | `RSP_LoadMatrix.cpp:10` | All s16/u16 |
| `uSprite` (u16 + u32 fields) | `gSP.cpp:2054` | u16/u32 |

### Fixes applied so far

1. **`convert.cpp:102-152`** — `UnswapCopyWrap`: on BE, copy bytes directly (NOP); on LE, keep byte-swap logic. Fixes ucode data string search ("RSP" scan) and texture loading.

2. **`GBI.cpp:398-409`** — CRC computation: on BE, byte-swap each 32-bit ucode word to LE before computing CRC, matching the precomputed LE CRC table.

3. **`Types.h`** — Added endian-aware accessor macros:
   - `E16_IDX(i)` — for `u16*` index access (LE: `^1`, BE: NOP)
   - `E16_ADDR(a)` — for byte-offset s16 reads (LE: `^2`, BE: NOP)
   - `E8_OFF(o)` — for byte offset access (LE: `^3`, BE: NOP)
   - `E_XOR(x)` — for template/runtime XOR contexts (LE: pass-through, BE: maps to 0)
   All use `__BIG_ENDIAN__` / `__BYTE_ORDER__` detection.

4. **`gSP.cpp`** — Replaced all 10+ XOR patterns:
   - Light position/attenuation (`addrShort+4..6 ^1`, `addrByte+3/7/14 ^3`)
   - CBFD light position (`addrShort+16..19 ^1`, `addrByte+12 ^3`)
   - Acclaim light data (`addrShort+0..7 ^1`, `addrByte+6..8 ^3`)
   - DMA vertex xyz/rgba (`address+0/2/4 ^2`, `address+6/7/8/9 ^3`)
   - CBFD vertex normal (`normalBase+0/1 ^3`)
   - LookAt vertex alpha (`DMAIO_address+128+index ^3`)
   - Matrix modify (`pData[i ^1]`)

5. **`gDP.cpp`** — Replaced 5 XOR patterns:
   - TMEM load 16-bit (`address ^2`)
   - TMEM load 16-bit with variable t (`(tb+i) ^t`, `(tb+i+1) ^t`)
   - TMEM load 16-bit fixed (`(tb+i) ^1`)
   - DMA texture offset (`tex_count ^1`)

6. **`GraphicsDrawer.cpp`** — Replaced 2 XOR patterns:
   - Depth buffer copy (`(ulx+x) ^1`)
   - Palette write (`i ^1`)

7. **`RSP_LoadMatrix.cpp`** — Replaced matrix row XOR (`j ^1`)

8. **`BufferCopy/WriteToRDRAM.h`** — Replaced pixel write XORs using `E_XOR(_xor)`

9. **`BufferCopy/ColorBufferToRDRAM.cpp`** — Replaced framebuffer fill XOR (`(x+y*VI.width) ^1`)

10. **`sdl_backend.cpp:101`** — Changed hardcoded `SDL_AUDIO_S16LE` to `SDL_AUDIO_S16`. RDRAM stores audio samples in host byte order. On LE hosts `SDL_AUDIO_S16` == `SDL_AUDIO_S16LE` (correct); on BE hosts it becomes `SDL_AUDIO_S16BE` (correct). Previously hardcoded LE caused each 16-bit sample to have reversed bytes on BE → static noise.

### Fixes applied this session (Jun 9)

#### 1. CRITICAL: CI4/CI8/CI16 palette reads used wrong byte offset on BE

**Symptom:** SM64 renders black screen. All textured surfaces use CI4 textures with palette lookup. Palette entries read as zero → transparent black → nothing visible.

**Root cause:** `TMEM` is `u64[512]`. The code `TMEM[idx] & 0xFFFF` reads a u64 then masks to low 16 bits. On LE this hits bytes 0-1 of the u64 (where the palette entry was written). On BE it hits bytes 6-7 (the last 2 bytes of the u64 — uninitialized/zero). The palette entry is always at bytes 0-1 of the u64 regardless of endianness.

**Fix:** Changed all 28 occurrences of `TMEM[idx] & 0xFFFF` to `((u16*)TMEM)[idx << 2]` — reads the correct u16 at bytes 0-1 on both LE and BE. `Textures.cpp`.

#### 2. gDPLoadBlock32: E_XOR(t) disabled TMEM interleave on BE

**Symptom:** `E_XOR(t)` with `t=1` or `t=3` maps both to 0 on BE, completely disabling the TMEM interleave pattern. Causes incorrect TMEM row ordering for block-loaded textures.

**Root cause:** Variable `t` is a TMEM interleave value (1 or 3), not an endian XOR. TMEM interleave works the same regardless of host endianness — it's a u16 index XOR, not a byte-order operation.

**Fix:** Reverted `E_XOR(t)` → `t`. `gDP.cpp:623,627`.

#### 3. DWordInterleaveWrap called on BE after NOP UnswapCopyWrap

**Symptom:** On odd TMEM rows during gDPLoadTile, `DWordInterleaveWrap` swaps adjacent u32 values within each u64. On LE this corrects a side effect of `UnswapCopyWrap`. On BE, `UnswapCopyWrap` is NOP (no byte reversal), so `DWordInterleaveWrap` corrupts the data on odd rows.

**Fix:** Guarded with `#if !defined(__BIG_ENDIAN__)` — skipped entirely on BE. `gDP.cpp:584-586`.

### Known non-issues (verified)

- **swapword double-swap** in CI4/CI8 TLUT path: TLUT load writes `swapword(RDRAM_u16)`, palette read does `swapword(extracted_u16)` in `RGBA5551_RGBA8888`. This double-swap cancels identically on both LE and BE. On LE, RDRAM stores host-order u16 = N64 value → swapword → TMEM → swapword → original. On BE, same flow (N64 value → swapword → TMEM → swapword → original). **No fix needed.**
- **swapword in direct 16-bit and 32-bit paths:** Same double-swap analysis applies. The swapword is a pixel-format conversion step, not an endian compensation.

### Status: Three more endian bugs fixed in GLideN64

**SM64 boots** (ucode recognized) but **black screen + static audio** status unchanged. All LE-specific XOR byte-swap patterns in critical SM64 rendering paths (gSP, gDP, GraphicsDrawer, RSP_LoadMatrix, BufferCopy) have been wrapped with endian-aware macros. The next compile/test cycle will reveal whether the black screen was caused by these XOR patterns or if additional issues remain.

Remaining files with XOR patterns (not used by SM64, defer for now):
- `ZSortBOSS.cpp` (Zelda ucode) — ~40 DMEM/RDRAM XOR patterns
- `ZSort.cpp` (Zelda ucode) — ~30 DMEM/RDRAM XOR patterns
- `F5Indi_Naboo.cpp` (Perfect Dark ucode) — ~19 DMEM XOR patterns

---

## Build system

Pass `-DPPC_DYNAREC=ON` to cmake. The `3rdParty/CMakeLists.txt` propagates:
- `PPC_DYNAREC=$<BOOL:${PPC_DYNAREC}>` to core Makefile
- `HOST_CPU=powerpc64 BIG_ENDIAN=1` (via `PPC_MAKE_FLAGS`) to all Makefile-based plugins when cross-compiling
- `override NO_ASM := 1` forced for all PPC targets (no assembly source available)

## Plugin compatibility (PPC64 BE)

| Plugin | Build | Runtime | Notes |
|--------|-------|---------|-------|
| `mupen64plus-core` | **OK** | **OK** | Pure/cached interpreter working; ucode crash fixed (RDRAMSize fix); PPC_DYNAREC enabled via cmake option but on hold |
| `mupen64plus-rsp-hle` | **OK** | **OK** | Endian-aware via `M64P_BIG_ENDIAN` memory macros (XOR-based accessors) |
| `mupen64plus-rsp-cxd4` | **OK** (scalar) | **OK** | SSE2 path auto-disabled; scalar fallback performs all ops |
| `mupen64plus-video-GLideN64` | **OK** | **IN PROGRESS** | SM64 boots, RDP otherMode and Vertex/PDVertex/SWVertex/Light structs fixed for BE. CI4 TLUT issue remains (same as Rice). See GLideN64 struct endian fixes section. | |
| `mupen64plus-video-rice` | **OK** | **IN PROGRESS** | Flicker-free rendering. SM64 uses CI16 textures (sz=2, NOT CI4), no palette needed. Black output is CI16→RGBA conversion bug on BE. Remaining: 2D-on-top z-ordering inverted (depth direction on BE), texture filtering issues. |
| `mupen64plus-video-parallel` | **OK** | **N/A** (no Vulkan on G5) | SSE2 guarded with `#ifdef __SSE2__`, has scalar fallback |
| `mupen64plus-input-raphnetraw` | **OK** | **OK** | No arch-specific code |
| `RMG-Audio` | **OK** | **OK** | Works after `SDL_AUDIO_S16LE`→`SDL_AUDIO_S16` (detects host endian automatically) |
| `mupen64plus-rsp-parallel` | **BLOCKED** | **N/A** | Unconditional SSE2 in `rsp_core.cpp` — disabled when `PPC_DYNAREC=ON` |

### Makefile warning fixes
Changed `$(warning ...)` to `$(info ...)` with "supported by RMG" for PPC blocks in:
- `mupen64plus-core/projects/unix/Makefile` (3 PPC blocks)
- `mupen64plus-rsp-hle/projects/unix/Makefile` (2 PPC blocks)
- `mupen64plus-rsp-cxd4/projects/unix/Makefile` (2 PPC blocks)

---

## Current focus

**Rice video plugin — Shadow CI4 texture investigation + GL_UNSIGNED_BYTE vs GL_UNSIGNED_INT_8_8_8_8_REV (Jun 19).**

### Shadow rendering investigation

SM64 shadow (dark circle under Mario) uses a CI4 texture. The shadow renders as black/invisible due to palette issues. Key diagnostic queries for stderr output:

| Symptom | Grep | What to look for |
|---------|------|-----------------|
| CI texture tile info | `TILE: fmt=0 sz=0` | `fmt=0 sz=0` = CI4, `sz=2` = CI16. Note `TLutFmt` value and `pal` (palette bank) |
| Palette interpretation | `TLUT_FMT=` | `RGBA16` vs `IA16`. If `IA16` but forced to `RGBA16` at `RDP_Texture.h:1100-1101`, palette colors are wrong |
| LOADTLUT fires? | `GBI_LOADTLUT:` | Shows `w0/w1` values. If no lines with this tag, `G_LOADTLUT` never fires |
| Palette entries | `TLUT: tileno=` | Shows raw `0x%04X` entries plus interpreted `RGBA=(r,g,b,a)` or `I= A=` |
| Vertex colors | `oglVtxColors[0..3]` | Shows `RR GG BB AA`. If channels are shifted (e.g., R reads GG or B reads AA), there's a byte-order issue in the TLITVERTEX chain |
| Post-draw pixels | `post-draw pixel` | Shows actual RGBA output at viewport center |
| Texture load formats | `TILE: fmt=0 sz=0` | For shadow specifically: `sz=0` means CI4 (needs palette), `TLutFmt` shows what palette format is assumed |
| Raw texture/palette bytes | `RAW[0..15]` / `PAL[0..31]` | Raw bytes as stored in RDRAM and g_wRDPTlut |

### CI4 shadow diagnostic dump

Enhanced in `RDP_Texture.h:1130-1167`:
- **CI_PAL** line: dumps 8 palette entries from `g_wRDPTlut` at the current tile's palette bank offset. Shows the actual palette data that will be used for CI texture lookup.
- **TLUT dump** (`RDP_Texture.h:1302-1316`): now shows `TLUT_FMT=RGBA16/IA16/NONE` and interprets each palette entry as RGBA5551 or IA16 values.
- **LOaDBLOCK/LOADTILE** diagnostics (`RDP_Texture.h:1424, 1646`): fires for CI format or TMEM>=0x100 loads, limited to 20 entries.

### GL_UNSIGNED_BYTE vs GL_UNSIGNED_INT_8_8_8_8_REV

On PPC64 BE, `GL_RGBA + GL_UNSIGNED_BYTE` for `glReadPixels` reads bytes in memory order: `[AA, RR, GG, BB]` from a native `0xAARRGGBB` framebuffer. This produces wrong RGBA output (R=AA, G=RR, B=GG, A=BB). The correct format for BE is `GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV` or `GL_RGBA + GL_UNSIGNED_INT_8_8_8_8`, which reads the native uint32 and maps components correctly.

For **texture upload** (`OGLTexture.cpp`): currently uses `GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV` which IS endian-agnostic — reads the uint32 `0xAARRGGBB` and maps B=byte3=BB, G=byte2=GG, R=byte1=RR, A=byte0=AA correctly on both LE and BE.

For **vertex colors** (`glVertexAttribPointer` with `GL_UNSIGNED_BYTE`): this is correct on BE because vertex colors are stored as separate bytes `[R, G, B, A]` in `g_oglVtxColors[]` (set at `RenderBase.cpp:898-901`), not as a packed uint32. Each byte maps to its component directly.

### Debug printf cleanup (Jun 19)

Reduced diagnostic limits across Rice plugin to reduce stderr spam:

| File | Change |
|------|--------|
| `Config.cpp:432-436` | Removed 3 config-reading debug fprintf calls (no longer needed) |
| `Config.cpp:678-679` | Removed GenerateCurrentRomOptions debug fprintf |
| `Render.cpp:643` | ZBUF_DISABLE limit 100000→100 |
| `Render.cpp:742` | TEXRECT limit 100000→100 |
| `Render.cpp:884` | TEXRECTFLIP limit 100000→100 |
| `RenderBase.cpp:875-878` | ZDBG limit 9 (unchanged, useful) |
| `RenderBase.cpp:1385-1397` | PROJMTX/MODVMTX limit 2 (unchanged, useful) |
| `RenderBase.cpp:1446` | VTX dump limit 1000→10 |
| `RenderBase.cpp:1569,1575` | CULL_BACK/CULL_FRONT limit 100000→100 |
| `RDP_Texture.h:1130` | TILE dump limit 1000→100 |
| `OGLGraphicsContext.cpp:135` | Removed `after OGLExtensions_Init glError` |
| `OGLGraphicsContext.cpp:159` | Removed duplicate renderer/version string |
| `OGLGraphicsContext.cpp:162-166` | Removed after-Init error checks (redundant) |
| `OGLGraphicsContext.cpp:231` | Removed "GL errors drained" message |
| `OGLRender.cpp:703` | Removed `pre-RenderFlushTris error` check (was always-0 noise) |
| `OGLTexture.cpp:26` | Removed `#include <cstdio>` (not used) |

**PPC64 dynarec — KSEG0/KSEG1 stripping in read_rmg_word/write_rmg_word (Jun 17).**

### Bug: Pure-ALU compiled PPC blocks never return to C dispatcher (FIXED Jun 17)

**Symptom:** Audio broken with dynarec (works with interpreter). SM64 stuck on PIF boot ROM delay loop at `0xA4000428`. Compiled PPC block containing the loop never returns to the C dispatcher — no interrupt check fires, no audio DMA is serviced.

**Root cause:** `genUpdateCount`'s BLELR compares Count against `next_interrupt` from the real interrupt queue. During PIF boot, the queue contains only `SPECIAL_INT` at `0x80000000` (a sentinel). Count starts at 0 and must advance by 2 billion units to catch up — impossible within a single compiled block execution. Pure-ALU backward branches (no load/store) never trigger the bypass path, so the compiled PPC code loops within the block indefinitely without ever returning to C.

The cached interpreter avoids this via `cycle_count` — a periodic counter checked at every branch that calls `gen_interrupt` even when no real interrupt is due. The PPC dynarec had no equivalent mechanism.

**Fix:** Before each `dyna_run`, clamp `next_interrupt` to `min(real_next_interrupt, Count + PPCD_CHECK_INTERVAL)` (10000 Count units ≈ ~5000 MIPS instructions). `genUpdateCount`'s BLELR fires when Count reaches this clamped value, forcing a return to C. After `dyna_run` returns, `next_interrupt` is restored from the r4300 struct (which bypass handlers may have updated). The interrupt check at line 451 only calls `gen_interrupt` when `Count >= real_next_interrupt`, preventing premature dispatch of `SPECIAL_INT`.

`ppc_dynarec.c:51-60,347-367`.

### Bug: GEN_MTLR/GEN_MTCTR/GEN_MFLR/GEN_MFCTR RS/SPR fields swapped (FIXED Jun 17)

**Symptom:** SIGILL at instruction [22] (offset 0x58) — RECOMP dump shows `0x7D0003A6`. Previously attributed to I-cache coherency erratum, but the real cause is a secondary SPR encoding bug in the Jun 13 fix.

**Root cause:** The Jun 13 fix for the SPR split encoding placed SPR[4:0] at `<<21` (C bits 25-21 = PPC bits 6-10 = RS field) and used `PPC_SET_RB` (`<<11`, PPC bits 16-20 = SPR[9:5] position) for the RS register. This produced `mtspr 0, r8` (SPR[4:0]=0, SPR[9:5]=0, RS=8) instead of `mtlr r0` (SPR[4:0]=8, SPR[9:5]=0, RS=0).

**Fix:** Reordered all four macros so that:
- RS/RT goes at `<<21` (PPC_SET_RD) — PPC bits 6-10
- SPR[4:0] goes at `<<16` (PPC_SET_RA position) — PPC bits 11-15
- SPR[9:5] goes at `<<11` (PPC_SET_RB position) — PPC bits 16-20

Same fix applies to all four macros (MTCTR, MFCTR, MTLR, MFLR) with SPR=9 for CTR and SPR=8 for LR.

`PowerPC.h:402-420,878-892`.

### Bug: KSEG0/KSEG1 not stripped in read_rmg_word/write_rmg_word (FIXED Jun 17)

**Symptom:** Dynarec hangs at IPL3 delay loop `0xA4000428`. All I/O register writes silently dropped — PI DMA never starts, SI status reads return open_bus garbage. Interpreter works fine for same code.

**Root cause:** `read_rmg_word()`/`write_rmg_word()` pass raw KSEG1 virtual addresses (e.g., `0xA460000C`) to `mem_get_handler()`, which indexes the handler table by `address >> 16`. The handler table is populated only for physical address indices (`0x0000`-`0x1FFF`). KSEG1 index `0xA460` was never registered, so every PI/SI/RI register access hit the `open_bus` NOP handler (reads return 0, writes are discarded). The interpreter's `r4300_write_aligned_word()` strips KSEG bits via `address &= 0x1FFFFFFC` before calling `mem_get_handler`, which is why the interpreter works.

**Fix:** Added `if ((vaddr & 0xC0000000) == 0x80000000) vaddr &= 0x1ffffffc;` to both `read_rmg_word` and `write_rmg_word`, matching the KSEG stripping in `r4300_read_aligned_word`/`r4300_write_aligned_word`.

`ppc_dynarec.c:990-1006`.

### Bug: `protect_framebuffers()` skipped for dynarec — RDRAM framebuffer writes not tracked (FIXED Jun 18)

**Symptom:** With PPC dynarec, SM64 renders black screen after resize. `rdpUpdate` returns OK, `renderBuffer` finds the buffer, but textured rect output is `(0,0,0,255)`. `m_vecAddress` is always empty → bCFB=0 `copyFromRDRAM(u32)` clears `m_pCurBuffer` and returns early → texture never updated from RDRAM.

**Root cause:** `protect_framebuffers()` in `fb.c:198-200` skips entirely when `emumode == EMUMODE_DYNAREC` with the comment "Dynarecs currently miss some of the read/writes needed for FBInfo". This means:
1. `fb->infos[]` never populated (all zeros)
2. `protect_framebuffers_write()` at line 40-43 early-returns because `fb->infos[0].addr == 0`
3. `gfx.fBWrite` is never called
4. `FrameBuffer_AddAddress()` → `RDRAMtoColorBuffer::addAddress()` never called
5. `m_vecAddress` stays empty
6. `copyFromRDRAM(u32)` with bCFB=0 at `RDRAMtoColorBuffer.cpp:318-324` clears `m_pCurBuffer = nullptr` and returns early
7. `_copyFromRDRAM()` never runs → texture is stale/zero → `renderBuffer()` draws black

The guard was added because x86/ARM dynarecs inline memory access in compiled code, bypassing C memory handlers. But the PPC dynarec always goes through C handlers (`dyna_mem()` → `write_rmg_word()` → `mem_write_handler()` → handler table), so it properly routes through `write_rdram_fb`.

**Fix:** Separated the two checks in `protect_framebuffers()`. API support check (`gfx.fBGetFrameBufferInfo && gfx.fBRead && gfx.fBWrite`) remains unconditional. The EMUMODE_DYNAREC skip is now guarded with `#ifndef PPC_DYNAREC`, so on PPC builds `protect_framebuffers()` runs normally regardless of `emumode`. `fb.c:198-206`.

**Note:** The interpreter worked because it uses `emumode=0/1` which doesn't trigger the skip.

### Bug: Pure-ALU loop at 0xA4000428 — Count never reaches next_interrupt (FIXED Jun 17)

**Symptom/root cause/fix:** See PPCD_CHECK_INTERVAL clamping above. Works after the fix.

### Note on bypass approach vs emit_64bit_call:
Since the GEN_MTCTR and GEN_MTLR macros are now correct, `bctrl` from compiled code may also work correctly. The bypass approach (genCallDynaMem/genCheckFP storing args to canary + BLR to C dispatcher) is the tested path and should be kept for now. If `bctrl` works in future testing, we can consider reverting to `emit_64bit_call` + `bctrl` (simpler, more efficient).

**Secondary: mupen64plus-video-rice** — SM64 rendering (pure interpreter already works). TLUT loading issue remains for CI4 textures.
**GLideN64** — SM64 boots, otherMode/Vertex structs fixed. Same TLUT CI4 loading issue as Rice: `G_LOADTLUT` never dispatched despite CI4 textures being visible.

### Rice plugin status

| Issue | Fix | Status |
|-------|-----|--------|
| Not listed in RMG | `extern "C" EXPORT` → `EXPORT` (removed redundant `extern "C"`); made `DXFrameBufferManager`/`OGLFrameBufferManager` virtual overrides inline in `FrameBuffer.h` (were undefined symbols on PPC64 `ld`) | **FIXED** |
| Bitfield structs use LSB-first order (x86 assumption) | Reversed field order within each 32-bit word under `#if __BIG_ENDIAN__` in `UcodeDefs.h` for all 22 command structs | **FIXED** |
| GL_DEPTH_BITS=0 — no depth buffer in context | `LoadConfiguration()` gated `ReadConfiguration()` behind `ReadIniFile()`. Restructured to call core API config reading unconditionally. Also clamped in `VidExt_GLSetAttr` and rice plugin `Initialize()`. | **FIXED** |
| SM64 z-ordering inverted (far objects in front) | Same as GL_DEPTH_BITS=0 fix — depth buffer now exists, z-test works | **FIXED** |
| SM64 Mario shadow missing | Same as GL_DEPTH_BITS=0 fix — shadow writes to depth buffer now works | **FIXED** |

### Bug: GL_DEPTH_BITS=0 — OpenGL context created without depth buffer (FIXED Jun 19)

**Symptom:** SM64 renders with inverted z-ordering (far objects in front of near ones) and missing Mario shadow. `glGetIntegerv(GL_DEPTH_BITS, ...)` returns 0.

**Root cause:** Rice plugin's `ConfigGetParamInt` returns `0` for `OpenGLDepthBufferSetting` despite config file having `OpenGLDepthBufferSetting = 16`. This value propagates through `CoreVideo_GL_SetAttribute(M64P_GL_DEPTH_SIZE, 0)` to `l_SurfaceFormat.setDepthBufferSize(0)`. When `VidExt_OglSetup` creates the OpenGL context, the format requests 0 depth bits → context has no depth buffer → all depth testing is a no-op.

**Root cause:** `LoadConfiguration()` in `Config.cpp:496` gates `ReadConfiguration()` behind `ReadIniFile()` (`RiceVideoLinux.ini`). If that INI file is missing or unreadable, the function returns FALSE early and `ReadConfiguration()` is never called — so all core config API values (including `OpenGLDepthBufferSetting`) are never read, leaving `options` fields at zero.

**Fix (three layers):**
1. **Config.cpp:496-518** — `LoadConfiguration()` restructured to call `ReadConfiguration()` regardless of `ReadIniFile()` success. The INI and core API configs are independent.
2. **VidExt.cpp:234-236** — `VidExt_GLSetAttr` clamps `Value` to 16 when `Value == 0` for `M64P_GL_DEPTH_SIZE`
3. **OGLGraphicsContext.cpp:68-69, 188-189** — Rice plugin clamps `depthBufferDepth` to 16 before passing to `CoreVideo_GL_SetAttribute`

**Result:** `OGLSETUP: reqDepth=16 defaultFormatDepth=24 widgetFormatDepth=24` → context created with `ctxFormatDepth=24` → `GL_DEPTH_BITS=24` → z-ordering correct, shadows visible.

### Root cause: bitfield packing direction on PPC64 BE

GCC on PPC64 BE packs bitfields **MSB-first** (first-declared field → most significant bits of the word), while x86 packs **LSB-first** (first-declared field → least significant bits). The rice plugin's 22 command structs in `UcodeDefs.h` were all designed for x86 LSB-first packing, where `cmd:8` is declared LAST in each word to land at the MSB (bits 24–31) — which after LE byteswap of the N64 BE u32 correctly reads byte 0 of the RDRAM word (the opcode byte).

On PPC64 BE MSB-first, declaring `cmd:8` last maps it to bits 7–0 (byte 3 of the BE word), which is never the opcode byte. The fix reverses every bitfield group within each 32-bit word on BE so `cmd:8` is declared FIRST, mapping to bits 31–24 = byte 0 = opcode.

All 22 structs (`Gwords`, `GGBI0_Tri1`, `GGBI2_Tri1`, `GGBI2_Tri2`, `GGBI0_Ln3DTri2`, `GGBI1_Tri2`, `GGBI2_Line3D`, `GGBI0_Vtx`, `GGBI1_Vtx`, `GGBI2_Vtx`, `GSetImg`, `GSetColor`, `GGBI0_Dlist`, `GGBI0_Matrix`, `GGBI0_PopMatrix`, `GGBI2_Matrix`, `GGBI0_MoveWord`, `GGBI2_MoveWord`, `GTexture`, `Gloadtile`, `Gsettile`, `Gtexrect`) and the `GSetColor` RGBA/fillcolor union all have `#if __BIG_ENDIAN__` alternates.

The main command dispatch (`currentUcodeMap[pgfx->words.w0 >> 24](pgfx)`) was unaffected because `>> 24` always reads the same byte (byte 0 of the BE word) regardless of host endianness — no endian compensation needed.

### Trace diagnosis that identified the root cause

Before the fix, trace output showed:
- `GetTexture` always returns `addr=0x00000000 fmt=0 sz=0 w=1 h=1` (null texture)
- Only one `ConvertTexture` call fires, for that null texture
- `EndUpdate` never fires (texture upload to OpenGL never attempted)
- Game freezes at `gDlistCount=77` with `screenUpdate=0 bScreenIsDrawn=1`

These symptoms are consistent with `GSetImg` reading all zero fields (because `width:12` — declared first in the struct — reads bits 31–20 on BE = the cmd+fmt byte region instead of the width region), so the texture address is set to 0x00000000 and never resolves to real texture data.

### Known fixes needed
1. ~~Wire rice into CMake build system~~ (DONE)
2. ~~Fix DXFrameBufferManager undefined vtable symbol~~ (DONE)
3. ~~Fix bitfield struct endianness in UcodeDefs.h~~ (DONE)
4. ~~Fix hardcoded `^0x2`/`^0x3` LE address XORs in LoadMatrix, Viewport, Light, FrameBuffer~~ (DONE)
5. ~~Verify rice renders SM64 correctly on PPC64 BE~~ (DONE — z-ordering, shadows, vertex colors, FiddledVtx rotation all verified working)

### Hardcoded LE address XORs (FIXED)

Despite the bitfield struct fix, the rice plugin had 4 critical remaining endian bugs: hardcoded `^0x2` and `^0x3` XOR operations on RDRAM byte addresses that assume LE byte-reversed storage within 32-bit words. On BE these read from wrong offsets, corrupting all matrix, viewport, light, and framebuffer data.

| File | Lines | Pattern | Fix |
|------|-------|---------|-----|
| `RSP_Parser.cpp` | 1937-1938 | `^0x2` in `LoadMatrix` | `^N64_XOR(2)` |
| `RSP_GBI1.h` | 344-352 | `^0x2` in `RSP_MoveMemViewport` (8x) | `^N64_XOR(2)` |
| `RSP_GBI1.h` | 295-297 | `^0x3` in `RSP_MoveMemLight` (3x) | `^N64_XOR(3)` |
| `FrameBuffer.cpp` | 510, 517 | `^0x3` in `TexRectToN64FrameBuffer` | `^N64_XOR(3)` |
| `RSP_GBI_Others.h` | 1428-1429 | `^0x2` in `PD_LoadMatrix_0xb4` | `^N64_XOR(2)` |
| `Debugger.cpp` | 311-312 | `^0x2` in matrix display (debug only) | `^N64_XOR(2)` |

The `N64_XOR(x)` macro (defined in `typedefs.h:26-30`) becomes `(0)` on BE and `(x)` on LE, making it a no-op where LE-specific byte compensation is not needed.

Additional fixes applied in the same session (Jun 17):

| File | Lines | Pattern | Fix |
|------|-------|---------|-----|
| `RenderBase.cpp` | 1702-1704, 1745-1746 | `^2` in `ProcessVertexDataDKR` (DKR vertex path) | `^N64_XOR(2)` |
| `ConvertImage16.cpp` | 132 | `^0x2` in 16-bit non-interleaved texture load | `^N64_XOR(0x2)` |
| `RSP_GBI1.h` | 237-239, 244-247, 254-255 | `^0x1`/`^0x3` in debug vertex dump | `^N64_XOR(0x1)`/`^N64_XOR(0x3)` |
| `RSP_GBI_Others.h` | 251-253, 257-258, 280-284 | `^1` in debug vertex dump | `^N64_XOR(1)` |

The ucode CRC identification (`RSP_Parser.cpp:738-751`) already had a `__BIG_ENDIAN__` guard (byte-swaps to LE before CRC computation), so the plugin will identify the correct ucode.

### Rice black screen diagnosis (Jun 17→18)

**RED_CLEAR test** (10 frames of full-screen `glClearColor(1,0,0,1)`) proved framebuffer and swap chain work end-to-end: post-swap center pixel reads `(255,0,0,255)` at (335,116) in the viewport center.

**Vertex data is valid:** `Vtx[0] pos=(10.944818,177.465866,2992.375000,3200.000000)`, `color=(97,255,0,149)` — non-zero green shade colors, correct N64 clip-space format (w=3200).

**Texture data IS correct:** `Convert555 tex=32x32 pitch=128 first4=0xFFFFFFFF 0xFFF7F7F7 0xFFF7F7F7 0xFFF7F7F7` — textures load with non-zero pixel data.

**Fragment shader runs but outputs FILL color:** `InitCombinerMode -> FILL` is taken for EVERY draw call. The FILL shader's `gl_FragColor = uFillColor` outputs the fill color. If the fill color is (0,0,0,0), the output is BLACK. The RED_CLEAR test RED border remains visible on one swap buffer outside the viewport, causing the alternating RED/BLACK "flashing" effect.

**Root cause identified:** `RDP_OtherMode` bitfield struct (`RSP_Parser.h:498-545`) has NO `#if __BIG_ENDIAN__` fix. On PPC64 BE MSB-first packing, `cycle_type` reads from physical bits 10-11 instead of bits 20-21, always yielding value 3 (CYCLE_TYPE_FILL). This makes the combiner take the FILL path for ALL draws, never the 1-cycle/2-cycle path needed for textured rendering.

### Rice vertex color bug (Jun 18)

**Symptom:** After RDP_OtherMode fix, SM64 renders visible content but Mario head has blue tint, logo shows only borders with blue tint, while PRESS START text has correct colors.

**Root cause:** `TLITVERTEX` and `LITVERTEX` in `typedefs.h:176-203` overlay `dcDiffuse` (uint32 `COLOR_RGBA` = `0xAARRGGBB`) with `{b, g, r, a}` byte struct (BGRA order). This is correct on x86 LE where `0xAARRGGBB` bytes in memory are [BB, GG, RR, AA], matching the {B,G,R,A} field order. On PPC64 BE with MSB-first struct packing, `0xAARRGGBB` bytes in memory are [AA, RR, GG, BB], so the overlay reads {Alpha, Red, Green, Blue}. The vertex color passed to OpenGL becomes:
- R = green byte → blue shift
- B = alpha byte → transparency holes

CI4/CI8 textures (like PRESS START) are unaffected because their fragment shader path doesn't modulate with vertex color (palette → direct RGBA → GL_BGRA+UNSIGNED_INT_8_8_8_8_REV which is endian-correct).

**Fix:** Added `#if __BIG_ENDIAN__` to swap byte order to `{a, r, g, b}` in both `TLITVERTEX` and `LITVERTEX`. `typedefs.h:182-203`.

### Rice FiddledVtx struct endian fix (Jun 18)

**Symptom:** 90° rotation in SM64 (and likely all GBI2 ucode games). All vertex positions are rotated 90°, because x and y coordinates are swapped. Additionally, z/flag and s/t texcoords are read from wrong byte offsets, and normal vector components map to wrong bytes.

**Root cause:** `FiddledVtx` in `typedefs.h:297-329` was designed for x86 LE's byte-swapped RDRAM. On LE, each 32-bit word in RDRAM has its bytes reversed, which swaps the two s16 fields within each word pair `((a<<16)|b)`. The struct's field order was deliberately "fiddled" to compensate for this LE byte reversal. On PPC64 BE with no byte reversal, EVERY short field was read from the wrong byte offset:

| Field | LE reads (correct) | BE reads (WRONG before fix) |
|-------|----|----|
| `vert.y` (offset 0) | N64 x₂ (word0 low) | N64 x₁ (word0 high) — x value! |
| `vert.x` (offset 2) | N64 y₁ (word0 high) | N64 y₂ (word0 low) — y value! |
| `vert.flag` (offset 4) | N64 flag₂ (word1 low) | N64 z₁ (word1 high) — z value! |
| `vert.z` (offset 6) | N64 z₁ (word1 high) | N64 flag₂ (word1 low) — flag value! |
| `vert.tv` (offset 8) | N64 t₂ (word2 low) | N64 s₁ (word2 high) — s value! |
| `vert.tu` (offset 10) | N64 s₁ (word2 high) | N64 t₂ (word2 low) — t value! |

Then `g_vtxNonTransformed[i].x = vert.x` and `g_vtxNonTransformed[i].y = vert.y` produced the rotated coordinates — `nonTransformed.x = N64 y`, `nonTransformed.y = N64 x`. On LE, the LE byte reversal within each word *again* swapped the reads back, making the struct accidentally correct. On BE without byte reversal, the struct reads the first N64 halfword at offset 0 (x), but the struct field name says `y`.

Additionally, the `norma` union (used for lighting) had the same un-guarded `{na, nz, ny, nx}` order as LE, which on BE maps `nx` to offset 15 (= byte `a`) instead of offset 12 (= byte `r`), corrupting all three normal components.

**Fix:** Added `#if __BIG_ENDIAN__` (lines 299-307, 334-338 in `typedefs.h`) with natural N64 byte order:
- `{x, y, z, flag, tu, tv}` matching in-RDRAM layout `[x_h,x_l, y_h,y_l, z_h,z_l, flag_h,flag_l, s_h,s_l, t_h,t_l]`
- `{nx, ny, nz, na}` matching byte positions `[r, g, b, a]`

The LE version is unchanged (still uses the "fiddled" compensating order).

### Post-fix verification (Jun 18)

**FiddledVtx.rgba fix confirmed working:**
- Vertex colors changed from `(255,97,149)/(255,35,54)` to `(149,149,97,255)/(54,54,35,255)` — R,G,B,A channels now read from correct byte positions
- Border colors changed from red to orangeish/yellowish — visually confirms channel mapping correction
- These colors are correct warm/amber tones matching SM64 light colors (L0=red down, L1=amber up, ambient=brown)

**CYCLE12 path confirmed working:**
- Fragment shader generates `texture2D(uTex0, ...).rgb * vertexShadeColor.rgb + 0` with `coverage=1.0` always
- RDP_OtherMode bitfield fix from Jun 17 is working — `InitCombinerMode -> CYCLE12` used (not FILL)

**FiddledVtx struct fix applied:**
- `#if __BIG_ENDIAN__` version now reads `{x, y, z, flag, s, t}` in natural N64 byte order instead of the LE-compensated "fiddled" order
- `norma` union also fixed for BE: `{nx, ny, nz, na}` match byte positions `{r, g, b, a}`
- Fixes 90° rotation, z/flag corruption, s/t texcoord swap, and normal vector component mapping
- `typedefs.h:297-347`

**CI4 textures render black — TLUT never loaded:**
- `DLParser_LoadTLut` is never dispatched: **no `LOADTLUT:` lines** in log despite CI4 tile (`fmt=0 sz=2 pal=0`)
- `g_wRDPTlut[]` remains all-zero → every CI4 index maps to palette entry 0 → `Convert555ToRGBA(0x0000)` = `(0,0,0,0)` → `texture * vertexColor` = `(0,0,0)` → opaque black per coverage=1.0
- Pixel reads all `(0,0,0,255)` — consistent with zero-texture CYCLE12 output

**90° rotation fixed** — root cause was FiddledVtx struct field order mismatch for BE; resolved by the `#if __BIG_ENDIAN__` alternates in `typedefs.h:297-347`.

### TLUT investigation

`DLParser_LoadTLut` is registered at `currentUcodeMap[0xF0]` for all ucode tables in `ucode.h`. The dispatch `pgfx->words.w0 >> 24` extracts byte 0, which is endian-safe on BE. Despite this:

- **No `GBI_LOADTLUT:` lines** from the dispatch-level dump (fired for opcode 0xF0 only, no counter limit) — SM64 never emits `G_LOADTLUT` for its CI4 textures
- Texture tile shows `fmt=0 sz=2 pal=0` — CI4 32x32 with palette bank 0
- `g_wRDPTlut[]` all-zero → `ConvertCI4_RGBA16` (in `ConvertImage.cpp:649`) reads palette from `tinfo.PalAddress = &g_wRDPTlut[0]` — produces transparent black pixels

This suggests SM64's logo CI4 textures may use a different texture loading mechanism (e.g., `gDPLoadTile` into TMEM with an embedded palette, or pre-expanded textures) that doesn't go through `G_LOADTLUT`.

### Bug: `gRDP.otherMode._u32[]` word order swapped on BE — FULL FIX REVERTED (Jun 18)

**Analysis:** `DLParser_RDPSetOtherMode()` stores `_u32[1] = w0 (High)`, `_u32[0] = w1 (Low)`. On BE the bitfield struct has High group in `_u32[0]`, Low group in `_u32[1]`, so every bitfield read (`gRDP.otherMode.*`) from the union returns swapped-word data. This includes `z_cmp`, `z_upd`, `cycle_type`, `blender`, `alpha_compare`, etc.

**Fix attempted:** Swap `_u32` indices on BE. This correctly reads all bitfields from the right words. **Result:** CYCLE12 combiner path (now correctly selected instead of FILL/COPY) generates white output for river and shadow — revealing a **secondary BE bug** in the combiner/shader generation code.

**Reverted:** The full `_u32` swap is reverted. Instead, a **targeted Z-buffer fix** is applied in `Render.cpp` — replacing all reads of `gRDP.otherMode.z_cmp`/`z_upd` with `(gRDP.otherModeL & Z_COMPARE/Z_UPDATE)` which always reads from the correct uint32 field. This fixes the see-through without affecting cycle_type/blender behavior.

**Affected lines in `Render.cpp`:**
- Line 640: `!gRDP.otherMode.z_cmp` → `!(gRDP.otherModeL & Z_COMPARE)` — disables Z-buffer when z_cmp=0
- Line 829: same pattern — restores Z-buffer state
- Line 1134: `gRDP.otherMode.z_cmp + ... > 0` → mask-based (Pilotwings hack)
- Line 1898: South Park Rally hack
- Line 1911-1912: `SetZCompare`/`SetZUpdate` — now read from `gRDP.otherModeL` directly

**Status:** See-through should be fixed. CYCLE12 combiner issue deferred — needs investigation into how the combiner/shader generator reads `cycle_type`, `blender`, `alpha_compare` on BE.

### GLideN64 struct endian fixes (Jun 18)

#### Bug: gDP.h OtherMode bitfield order on BE

**Symptom:** GLideN64 SM64 always uses CYCLE_TYPE_FILL (cycleType=3) regardless of actual RDP mode word. All textured surfaces render as black fill.

**Root cause:** Same class as Rice's RDP_OtherMode bug — `gDP.h`'s `OtherMode` bitfield struct had field order for x86 LE (LSB-first packing). On PPC64 BE with MSB-first packing, `cycleType` at bits 26-27 (where LE LSB-first places it) was mapped to completely different physical bits, always reading value 3 (FILL).

**Fix:** Added `#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__` with fields declared in reverse bit order within each 32-bit group, mirroring the N64 RDP bit layout. Also groups reversed: mode1 (low word) declared before mode0 (high word) on BE. `gDP.h:145-238`.

#### Bug: GBI.h Vertex/PDVertex/SWVertex/Light struct "fiddled" field order on BE (Jun 18)

**Symptom:** With OtherMode fix, SM64 showed Mario head shape against red background but 90° rotated, with black vertex colors — identical to Rice's FiddledVtx bug.

**Root cause:** `GBI.h`'s `Vertex`, `PDVertex`, `SWVertex`, and `Light` structs have deliberately "fiddled" field order that compensates for LE byte-swapped RDRAM within each 32-bit word. On BE with no byte swap, this causes every s16 field pair to read from wrong halves of each word:

| Field | LE reads (correct via byte swap) | BE reads (wrong without swap) |
|-------|----|----|
| `y` at offset 0 | N64 x₂ (low) | N64 x₁ (high) — y gets x! |
| `x` at offset 2 | N64 y₁ (high) | N64 y₂ (low) — x gets y! |
| `flag` at offset 4 | N64 z₁ (high) | N64 flag₂ (low) — flag gets z! |
| `z` at offset 6 | N64 flag₂ (low) | N64 z₁ (high) — z gets flag! |

The fix adds `#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__` to all four structs, mapping fields in natural N64 byte order.

**`z`/`flag` gotcha:** The N64 F3DEX2 vertex format is `x, y, z, flag, s, t` (z BEFORE flag). Initial fix had `flag, z` (mirroring LE struct's `flag, z` position) — this caused z values to be read as flag (setting "no screen clip" → geometry extends beyond viewport → red desktop garbage). Corrected to `z, flag`. `GBI.h:420-630`.

#### Impact on SM64 rendering

With all three struct fixes (convert.h, gDP.h, GBI.h):
- `convert.h` — byte-swap NOP + correct RGBA8888 byte order (committed `3ea29dad`)
- `gDP.h` — otherMode bitfield (committed `5b2b8067`)
- `GBI.h` — Vertex/PDVertex/SWVertex/Light (uncommitted)

The `z`/`flag` correction should restore Mario head shape at correct orientation with visible vertex colors.

**REVISED Jun 19:** SM64 logo textures are CI16 (sz=2), NOT CI4 (sz=0). CI16 stores RGBA5551 directly — no palette needed. The "black CI4" assumption was wrong. CI16→RGBA conversion on BE is the real suspect.

#### Bug: RDRAMtoColorBuffer.cpp hardcoded `_xor=1` without E_XOR (Jun 18)

**Symptom:** 16-bit framebuffer reads from RDRAM read wrong u16 pixels on BE because adjacent u16 indices within each 32-bit word are XOR-swapped.

**Root cause:** `RDRAMtoColorBuffer.cpp` `_copyBufferFromRdram` and `_copyPixelsFromRdram` use `^ _xor` (with `_xor=1` for 16-bit mode) on RDRAM index. Unlike `WriteToRDRAM.h`'s `writeToRdram` template which wraps `_xor` with `E_XOR()`, these functions use raw `_xor` without endian guard. On BE, `_xor=1` swaps u16 indices, reading wrong pixels.

**Fix:** Changed `^ _xor` to `^ E_XOR(_xor)` at lines 111 (read index) and 145 (write index) in `RDRAMtoColorBuffer.cpp`.

---

## PPC dynarec canary debugging

`volatile uint32_t dyna_canary[16]` inserted at points to isolate where the OOT hang occurs. Printed by SIGALRM handler on timeout. Additionally, `emit_64bit_call` stores the constructed target address intermediates to canary slots for debugging.

### Canary slot assignments

`dyna_canary[48]` at 8-byte aligned global, r31 points to it in compiled code.

| Slot | Set by | Expect | Meaning |
|------|--------|--------|---------|
| `[0]` | `dyna_run()` C code before asm | `1` | `dyna_run` entered |
| `[3]` | `dyna_mem()` entry | `1` | dyna_mem reached |
| `[8]` | `dyna_test()` body (`0xFE`), `dyna_mem()` prologue (`0xBE`) | varied | C-code-only confirm function body entered |
| `[9]` | Trampoline before asm (`0xAA`), dyna_mem entry (`0xDE`), trampoline after asm (`0xFF`) | varied | `0xDE` = dyna_mem entered (C-code-only slot) |
| `[10]` | `genCallDynaMem` `li 0, 0xCC; stw 0, 40(r31)` | `0xCC` | 1st memory access before bctrl |
| `[11]` | `genCallDynaMem` `li 0, 0xEE/0xDD; stw 0, 44(r31)` | `0xDD`/`0xEE` | `0xDD` = bctrl returned (1st), `0xEE` = subseq call before bctrl |
| `[12]` | `emit_64bit_call` `li 0, 0xBB; stw 0, 48(r31)` | `0xBB` | Reached mtctr step |
| `[13]` | `emit_64bit_call` `stw r12, 52(r31)` | low32(target) | r12 right before `bctrl` — actual CTR value |
| `[20]` | `dyna_test` return value (direct C call test) | `1` | low32 of return from direct C call |
| `[21]` | `dyna_test` return value (direct C call test) | `1` | return value |
| `[22]` | Direct C call test marker | `0xBE` | Confirmed returned from direct C call |
| `[23]` | Direct C call test marker | `0xCA` | Before direct C call |
| `[24]` | Direct C call test result | varied | low32 of dyna_test result (direct C call) |
| `[25]` | Direct C call test marker | `0xBE` | Direct C call returned successfully |
| `[30]` | `emit_64bit_call` `stw r1, 120(r31)` | low32(r1) | r1 (stack pointer) before mtctr — ABI integrity |
| `[31]` | `emit_64bit_call` `stw r2, 124(r31)` | low32(r2) | r2 (TOC pointer) before mtctr — ABI integrity |
| `[32]` | `emit_64bit_call` `li 0, 0xCC; stw 0, 128(r31)` | `0xCC` | Right before bctrl instruction |
| `[33]` | `emit_64bit_call` `li 0, 0xDD; stw 0, 132(r31)` | `0xDD` | Right after bctrl returned (only set if bctrl returns) |
| `[15]` | `emit_64bit_call` `mfctr 11; stw 11, 60(r31)` | low32(CTR) | CTR readback after `mtctr` — 0 before isync fix (SPR rename), should equal r12 after isync |
| `[34:35]` | Pre-stored at runtime before `dyna_run()` | 64-bit addr | Address of `dyna_check_cop1_unusable` (loaded via `ld` by emit_64bit_call) |
| `[36:37]` | Pre-stored at runtime before `dyna_run()` | 64-bit addr | Address of `dyna_test` (loaded via `ld` by emit_64bit_call) |
| `[38:39]` | Pre-stored at runtime before `dyna_run()` | 64-bit addr | Address of `dyna_mem` (loaded via `ld` by emit_64bit_call) |
| `[40:44]` | `genCallDynaMem`/`genCheckFP` bypass wrapper | varied | Pending call args (vaddr, word, memType/tgt, next_pc, byteLen) |
| `[45]` | `genCallDynaMem`/`genCheckFP` bypass wrapper | `1` (dyna_mem) or `2` (cop1) | Pending call target function selector |
| `[46]` | C dispatcher after `dyna_run()` returns | `0` | Pending call dispatched — cleared by C after servicing |

### Critical slot conflict (FIXED)

**dyna_canary[1]** was shared between `dyna_mem()` C code setting `0xDEAD` and `emit_64bit_call` stw `r12, 4(r31)`. Since the stw runs BEFORE dyna_mem is called, canary[1] always showed the 64-bit address low half, never 0xDEAD. **Fix:** `dyna_mem()` moved its entry marker to canary[9] (C-code-only slot, not touched by compiled code).

### Files
- `ppc_dynarec.c`: `dyna_canary[48]` global, C-code stores [0]/[23]/[24]/[25], `dyna_mem()` stores [3]/[8]/[9], trampoline loads r31 and stores [9], alarm handler prints all, print in `dynarec()` loop. Pre-stores function addresses at [34..39] before each `dyna_run()`.
- `MIPS-to-PPC.c`: `emit_64bit_call()` uses `EMIT_LD` from pre-stored canary slots for known functions (dyna_test→[36], dyna_mem→[38], cop1→[34]); stw+ld+sync fallback for other targets. Emits diagnostic stores to [10]/[11]/[12]/[13]/[30]/[31]/[32]/[33].

### SIGALRM timeout + canary diagnostics (Jun 10)

Since the CANARY line is only printed AFTER `dyna_run()` returns (it hangs before the print), added a SIGALRM timeout mechanism:

1. **`#include <signal.h>`, `#include <unistd.h>`** — for `signal()`/`alarm()`
2. **`dyna_alarm_handler()`** — SIGALRM handler: prints full canary dump and calls `_exit(1)`
3. **PRE-RUN CANARY print** — before each `dyna_run()`, prints the canary state left by the previous run
4. **Canary reset** — `memset` to zero before each `dyna_run()` call
5. **Alarm around `dyna_run()`** — `alarm(5)` before, `alarm(0)` + `signal(SIGALRM, SIG_DFL)` after

When the first `dyna_run()` hangs, after 5 seconds the handler fires and shows where it stuck.
key file `ppc_dynarec.c:271-302` alarm + canary diagnostic wrapping dyna_run().

### Jun 11 session

**Changes made:**
1. **`dyna_canary` expanded 16→40** — to hold ABI diagnostic slots and decisive test markers
2. **emit_64bit_call r1/r2 diagnostic** — stores r1 to canary[30], r2 to canary[31] right before mtctr, 0xCC/0xDD to [32]/[33] around bctrl
3. **Decisive test: skip C call on first memory access** — genCallDynaMem(mem_call_seq==1) emits `LI(0, 0xBB); STW(0, canary[36]); LI(3, 0)` instead of emit_64bit_call()
4. **emit_64bit_call switched to sldi+or** — replaced stw+ld+sync with register-based 64-bit construction (fails on PPC970: sldi doesn't produce correct shift — canary[15]=0x07FF0000)
5. **GEN_RLDICR bits 28-29 FIXED** — were `00` instead of `11` (RLDICR variant), causing PPC970 to decode rldicr as unknown instruction. Also fixed GEN_RLDICL bits 28-29 → `10`.
6. **emit_64bit_call switched to ld-from-canary** — loads 64-bit address from pre-stored canary slots via single `ld` instruction. Fallback path uses stw+ld+sync (verified correct on PPC970). Known functions (dyna_test→[36], dyna_mem→[38], cop1→[34]) use ld; others use stw+ld+sync fallback.
7. **Direct C call test** — dyna_run() now calls dyna_test() from C code before the asm trampoline (canary slots 20-25). Distinguishes C-context issue from compiled-code-context issue.
8. **dyna_canary expanded 40→48** — to hold pre-stored function addresses at [34..39].

### Jun 12 session

**Changes made:**
1. **`GEN_ISYNC` macro added to `PowerPC.h`** — follows `GEN_SYNC` pattern using `PPC_FUNC_ISYNC=150` in X-form.
2. **`EMIT_ISYNC` macro added to `Recompile.h`** — wraps `GEN_ISYNC` like other `EMIT_*` macros.
3. **`isync` inserted between `mtctr` and `mfctr` in `emit_64bit_call()`** (`MIPS-to-PPC.c:81-83`). Forces context synchronization so PPC970 SPR rename is committed before mfctr reads back CTR.

**Theory:** If canary[15] still reads 0 after isync, the issue is NOT a rename stall but rather stale I-cache at the mtctr instruction position (dcbf+icbi not reaching that line). If canary[15] reads the correct value, SPR rename was indeed the problem — but bctrl might still fail since `mtctr` + `bctrl` without isync is also subject to the same rename issue.

### Jun 14 session

**Changes made:**
1. **genCallDynaMem bypass** (`MIPS-to-PPC.c`) — Replaced `emit_64bit_call()` + `bctrl` with wrapper that stores args to `canary[40..45]`, emits `ld r0,20(r1)` + `mtlr r0` + `blr` to return to C dispatcher.
2. **genCheckFP bypass** (`MIPS-to-PPC.c`) — Same pattern for COP1 unusable check.
3. **Pending call dispatcher** (`ppc_dynarec.c:361-386`) — After `dyna_run()` returns, checks `canary[46]` and dispatches deferred C call (dyna_mem or dyna_check_cop1_unusable) from C context. Updates MIPS PC from result.
4. **D/I-cache combined loop** (`Recomp-Cache.c:13-30`) — Merged separate `dcbf` loop + `icbi` loop into single interleaved `FlushCacheRange()` to prevent PPC970 prefetcher from re-validating D-cache between loops (erratum: `icbi` skips invalidation if D-cache line is valid).
5. **Updated canary[] slots** — Added pending-call slots [40..46] to the table and debug prints.

**Test result:** SIGILL at instruction [22] (offset 0x58) — RECOMP dump shows `0x7D0003A6`. Root cause: the SPR split encoding macros had a SECONDARY bug — SPR[4:0] was placed at `<<21` (RS field) instead of `<<11` (SPR[4:0] field), and `PPC_SET_RB` (which uses `<<11`) was used for RS. This produced `mtspr 0, r8` instead of `mtlr r0` (= `0x7C0803A6`). **Fix:** Reordered all four SPR macros: RS at `<<21` (PPC_SET_RD), SPR[4:0] at `<<16`, SPR[9:5] at `<<11`. `PowerPC.h:873-892,402-420`.

### Jun 18 session

**Changes made:**
1. **Verification:** FiddledVtx.rgba fix working — vertex colors changed from `(255,97,149)/(255,35,54)` to `(149,149,97,255)/(54,54,35,255)`. Border colors changed from red to orangeish/yellowish.
2. **Verification:** CYCLE12 path confirmed working (not FILL) — RDP_OtherMode bitfield fix from Jun 17 is effective.
3. **TLUT never loaded:** `DLParser_LoadTLut` is never dispatched — no `LOADTLUT:` lines despite CI4 textures being rendered. Root cause of black textured interior.
4. **Missing `}` brace fix** (`RDP_Texture.h:1276`): `} }` → `} } }` — the `{ static int tl_dump` block had 3 opening braces but only 2 closing ones, consuming the function's closing `}` and causing cascade errors in all subsequent function definitions across RDP_Texture.h, RSP_GBI0.h, RSP_GBI1.h.
5. **GBI_LOADTLUT entry dump added** (`RSP_Parser.cpp:922`): fires for ANY dispatch with opcode 0xF0, no counter limit. Confirms definitively whether SM64 emits G_LOADTLUT commands.
6. **TLUT dump relaxed** (`RDP_Texture.h:1271-1276`): changed from `dwCount == 16` to `dwCount > 0`, limit 8 entries.
7. **LOADTLUT entry dump** (`RDP_Texture.h:1220-1222`): `fprintf(stderr, ...)` at function start with `++tl_cnt <= 8` limit. Shows both `w0`, `w1`, and `options.bUseFullTMEM` value.

### Jun 19 session

**Changes made:**
1. **PALFIX reverted** (`RDP_Texture.h:1402-1432`): Removed the broken PALFIX code that tried to populate `g_wRDPTlut[]` from LOADBLOCK data at TMEM `>= 0x100`. The fix never triggered because ALL SM64 LOADBLOCK entries have `dwTMem=0x0`.

2. **Key finding: SM64 uses CI16, not CI4** — Texture tile log shows `fmt=0 sz=2 pal=0` (CI format with 16-bit size), NOT `sz=0` (4-bit CI4). CI16 textures store 16-bit RGBA5551 color values directly — each texel IS the color, no palette lookup needed. `g_wRDPTlut[]` being all-zero is irrelevant for CI16 textures. The "black texture" output is NOT caused by palette issues.

3. **LOADBLOCK analysis from G5 log:**
   - ALL CI texture loads go to `dwTMem=0x0`, never to `0x100`
   - `sz=2` (16-bit) not `sz=0` (4-bit) → CI16 format
   - RAW texture data `FF FF F7 BD F7 BD...` shows valid RGBA5551 values
   - `TLutFmt=0` (TLUT_FMT_NONE) → code forces to `TLUT_FMT_RGBA16`
   - `bUseFullTMEM=0` → palette NOT read from TMEM either
   - Vertex colors are non-zero (e.g., `oglVtxColors = 00 F1 00 FF`) — confirmed valid color data

4. **New bug: "2D texture on top" rendering (z-ordering inverted)**
   - **SM64:** rug on floor appears on top of everything (rendered in top layer)
   - **OOT:** road/ground appears on top of Link and NPCs — should be behind
   - **Root cause:** `GL_DEPTH_BITS=0` — OpenGL context created without depth buffer. Rice plugin's `ConfigGetParamInt` returns 0 for `OpenGLDepthBufferSetting` despite config file having 16. All z-testing is a no-op.
   - **Fix:** Clamp depth to min 16 in `VidExt_GLSetAttr` (VidExt.cpp:234-236) and in rice plugin's Initialize functions (OGLGraphicsContext.cpp:68-69, 188-189). `OGLSETUP: ctxFormatDepth=24 → GL_DEPTH_BITS=24` verified.

5. **otherMode field verification (BE bitfield structs ARE correct):**
   - `_u32[0]=0x0f0a4000 _u32[1]=0x00b82c00` with BE struct:
     - cycle_type = bits 21-20 of _u32[1] = binary 11 = 3 (FILL) ✓ (matches log)
     - text_filt = bits 13-12 = binary 10 = 2 (BILERP) ✓
   - `_u32[0]=0x00552078 _u32[1]=0x00882c00`:
     - z_cmp = bit 4 of _u32[0] = 1 ✓ (matches log)
     - z_upd = bit 5 = 1 ✓ (matches log)
   - Conclusion: the `#if __BIG_ENDIAN__` bitfield struct fix from Jun 17 IS working correctly

6. **PALFIX diagnostic dump removed** — the CI texture diagnostic (LOADBLOCK info for CI format) is kept for future texture analysis

**Root cause of "black" CI16 textures narrowed:**
- CI16 doesn't need palette — that IS the color data
- The RGB values in RAW texture data are non-zero
- The combiner IS running (prog=4, texture name=1)
- Vertex colors ARE non-zero
- But output pixel is `(0,0,0,255)`
- Possible: CI16→RGBA conversion has a bug on BE (the CI16 texel RGBA5551 value is read incorrectly)

**Next investigation targets:**
1. `ConvertImage.cpp` CI16 conversion path — check if `sz=2` (16-bit) CI texture conversion reads RGBA5551 correctly on BE
2. Depth/z-ordering in `Render.cpp` — investigate `z_cmp`/`z_upd` handling and depth comparison direction
3. Verify whether `bUseFullTMEM` should be force-enabled for BE (SM64 loads palette data embedded in same LOADBLOCK to TMEM=0x0)

## Future: generic PPC dynarec architecture

The current dynarec targets PPC64 BE (ELFv2) only. To support PPC32 BE (Mac G4/GC/Wii) or PPC64 LE (POWER8+), the recommended architecture is:

### Core shared files (ABI-independent)

| File | Role |
|------|------|
| `PowerPC.h` | PPC instruction encoding macros — already bit-level, endian-independent |
| `MIPS-to-PPC.c` | MIPS→PPC translator, register allocator calls, memory/COP0 helpers |
| `Register-Cache.c` / `.h` | GPR/FPR register allocator |
| `Recomp-Cache.c` / `.h` | Recompiled code cache with LRU |
| `FuncTree.c` / `.h` | BST for recompiled block lookup |
| `Recompile.c` / `.h` | Block compilation, jump fixup, `genJumpPad()` |

### Variant-specific trampoline/call-emission files

| Variant | File(s) | Key differences from PPC64 BE |
|---------|---------|-------------------------------|
| PPC64 BE | `ppc_dynarec.c` (current) | 64-bit `ld`/`std`, MIPS reg offset `i*8+4` (high 32 of 64-bit slot on BE), r2=TOC, r13=TP, `r1+20` LR save, `emit_64bit_call` uses `rldicl`/`sldi` |
| PPC64 LE | `ppc_dynarec_64_le.c` + variant of `MIPS-to-PPC.c` for reg offset | Same pointer size, but MIPS reg offset = `i*8+0` (low 32), `emit_64bit_call` ok, cache line = 128 bytes (POWER8+), `dcbf`/`icbi` loop stride changes |
| PPC32 BE | `ppc_dynarec_32.c` + variant headers | 32-bit `lwz`/`stw` for pointers, MIPS reg offset `i*4` (32-bit array), `emit_64bit_call` unnecessary (32-bit range), r13=TP only, LR save at `r1+4`. Reference: dot64 N64 emulator for GC/Wii (https://github.com/AirGamez/dot64) which already has a working PPC32 BE dynarec |

### Strategy

- Define a common interface (e.g. `PPC_DYNAREC_INIT`, `PPC_DYNAREC_RUN`, trampoline setup, `emit_64bit_call` signature) that all variants implement
- Put variant-specific register assignments (reserved regs, MIPS reg offset, LR offset) in a small `ppc_dynarec_variant.h` that each variant provides
- `MIPS-to-PPC.c` includes the variant header and uses macros like `MIPS_REG_OFFSET(i)` and `MIPS_REG_BASE_REG` so the same translator code works across BE/LE and 32/64
- Cache line size and `dcbf`/`icbi` stride come from the variant header too
