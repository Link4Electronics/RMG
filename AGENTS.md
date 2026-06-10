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

The `PPC_SET_SPR` macro itself (`(spr & 0x3FF) << 11`) is **correct** for standard POWER-PC split SPR encoding — it properly places `SPRN[0]` at physical bit 11 and `SPRN[9]` at physical bit 20, matching the ISA specification.

**Verification:**
- Bad: `0x7D8903A6` = `mtspr 288, r12` (Xenon SPR)
- Good: `0x7D804BA6` = `mtspr 9, r12` = `mtctr r12` (standard)

### Bug: FAILSAFE_REC_NO_VM not set (FIXED)

**Symptom:** Fast memory path (`genCallDynaMemVM`) emits direct PPC loads/stores at N64 KSEG1 addresses `(addr & 0x1FFFFFFF) | 0x40000000` — unmapped on Linux.

**Fix:** Set `failsafeRec |= FAILSAFE_REC_NO_VM` in `ppc_dynarec_init()` to force slow path via `dyna_mem()` C function. `ppc_dynarec.c`.

### Bug: Register allocator uses r2/r13 on PPC64 ELFv2 (FIXED)

**Symptom:** r2=TOC pointer, r13=thread pointer. If allocator maps a MIPS GPR to r2/r13, all C function calls from recompiled code have corrupted TOC/DSA.

**Fix:** Set `availableRegsDefault[2]=0` and `availableRegsDefault[13]=0` in `Register-Cache.c`. Only r24-r31 available for MIPS GPR mapping.

### Bug: D-cache/I-cache coherency (FIXED — REVERTED FOR PPC970)

**Symptom:** First compiled block hangs before any dyna_mem call. No debug output after `dyna_run` entry.

**Root cause:** PPC970 (G5) erratum — `icbi` does not invalidate I-cache when the D-cache line is still valid (clean but present). `dcbst` writes back but keeps the line valid; `dcbf` writes back AND invalidates, which is what `icbi` requires on PPC970.

**Original fix (for Xenon):** Changed `dcbf` → `dcbst` — correct on Xenon where `dcbf` is a hint, but WRONG on PPC970 where `dcbf` works and `dcbst` breaks icbi.

**PPC970 fix:** Changed `dcbst` → `dcbf`. Also added `sync` before `isync` in ICInvalidateRange (kept). `Recomp-Cache.c:18`.

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

### Bug list reminder

| Bug | File | Lines | Status |
|-----|------|-------|--------|
| `bl` range exceeded | `MIPS-to-PPC.c` | emit_64bit_call(), 14 call sites | FIXED |
| `bl 4` GAS vs Xenon | `ppc_dynarec.c` | 97-136 | FIXED |
| **Xenon SPR encoding** | `PowerPC.h` | **394, 401, 842, 849** | **FIXED (root cause)** |
| FAILSAFE_REC_NO_VM | `ppc_dynarec.c` | 488 | FIXED |
| r2/r13 register corruption | `Register-Cache.c` | 13-17 | FIXED |
| D/I-cache coherency | `Recomp-Cache.c` | 13-29 | FIXED |
| `get_physical_addr()` | `ppc_dynarec.c` | 533-544 | FIXED |
| `genJumpPad()` LR restore | `Recompile.c` | 173-174 | FIXED |
| `lwz` LR on PPC64 | `MIPS-to-PPC.c` | 1095, 1912 | FIXED |
| **Backward branch BLR** | `MIPS-to-PPC.c` | 219-222 | FIXED |
| **Fallthrough address ternary** | `MIPS-to-PPC.c` | 209-210 | FIXED |
| **Stale `next_interupt` after interrupt** | `ppc_dynarec.c` | 246-247 | FIXED |
| **`decodeNInterpret` sentinel** | `ppc_dynarec.c` | 732 | FIXED |
| **GEN_BNE/GEN_BNELR BO encoding** | `PowerPC.h` | 565, 1125 | FIXED |
| **MTFSFI immediate field position** | `PowerPC.h` | 1021-1026 | FIXED |
| **HEAP_PARENT shift (>>2→>>1)** | `Recomp-Cache.c` | 46 | FIXED |
| **Split asm blocks in dyna_run** | `ppc_dynarec.c` | 98-137 | FIXED |
| **branch() BO/BI values (CR4 + polarity)** | `MIPS-to-PPC.c` | 197-205 | FIXED |
| **start_new_block() stale-code** | `MIPS-to-PPC.c` | 112-116 | FIXED |
| **rldicl after lis** | `PowerPC.h`, `Recompile.h` | 627-636, 165-166 | FIXED |
| **branch() bo/nbo inversion (all six conditions)** | `MIPS-to-PPC.c` | 197-205 | **FIXED** |
| **r23/DYNAREG_ZERO overwritten by canary trampoline** | `ppc_dynarec.c`, `MIPS-to-PPC.c`, `Register-Cache.c` | 134-136, 1974-1990, 18 | **FIXED** |

### Known issues

1. **Floating-point control** — `fesetround()` / `mtfsf` / `mffs` path needs runtime verification that rounding mode is set correctly for N64 FE_TOWARDZERO emulation.
2. **No VMX128 in the CPU dynarec** — The recompiler emits only scalar PPC (add, lwz, stw, rlwinm, etc.). LVX/STVX/VOR macros are standard AltiVec and work on both G4/G5 and Xenon.

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
| `mupen64plus-video-GLideN64` | **OK** | **DEFERRED** | SM64 boots but black screen; deferred in favor of rice (OpenGL 2.0). LE-specific XOR byte-swap pattern fixes applied but untested. | |
| `mupen64plus-video-rice` | **OK** | **PENDING** | Listed in RMG after fixing undefined `DXFrameBufferManager` vtable symbol (inline stubs in `FrameBuffer.h`). Rendering TBD. |
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

**Primary: PPC64 dynarec debugging** — diagnosing hang in first `dyna_run()` call.
**Secondary: mupen64plus-video-rice** — SM64 rendering (pure interpreter already works).
**GLideN64** — deferred in favor of rice (OpenGL 2.0 compatibility on G5).

### Rice plugin status

| Issue | Fix | Status |
|-------|-----|--------|
| Not listed in RMG | `extern "C" EXPORT` → `EXPORT` (removed redundant `extern "C"`); made `DXFrameBufferManager`/`OGLFrameBufferManager` virtual overrides inline in `FrameBuffer.h` (were undefined symbols on PPC64 `ld`) | **FIXED** |
| Bitfield structs use LSB-first order (x86 assumption) | Reversed field order within each 32-bit word under `#if __BIG_ENDIAN__` in `UcodeDefs.h` for all 22 command structs | **FIXED** |

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
4. Verify rice renders SM64 correctly on PPC64 BE (next compile/test cycle)
5. If black screen/corruption remains, investigate:
   - `RSP_Parser.cpp` code that reads `gfx->words.w0` with `& 0xFF`, `>> 12`, `>> 16`, etc. — these shift operations on the u32 value are endian-safe
   - RDRAM endian access via `g_pRDRAMu32[pc>>2]` (should be correct, uses host byte order)
   - Any remaining struct-style access to RDRAM data not covered by the Gfx union
6. Once video output works, optionally return to PPC dynarec testing

## PPC dynarec canary debugging

`volatile uint32_t dyna_canary[16]` inserted at 7 points to isolate where the OOT hang occurs. Printed in `dynarec()` loop when `dbg_iter <= 50`.

| Slot | Set by | Value | Meaning |
|------|--------|-------|---------|
| `[0]` | `dyna_run()` C code before asm | `1` | `dyna_run` entered |
| `[3]` | `dyna_mem()` entry | `1` | dyna_mem reached |
| `[4]` | `dyna_mem()` after switch | `1` | dyna_mem completed |
| `[5]` | `dyna_run()` C code after asm | `1` | asm block returned |
| `[10]` | Compiled PPC code before 1st `bctrl` | `0xCC` | 1st genCallDynaMem reached |
| `[11]` | Compiled PPC code after 1st `bctrl` | `0xDD` | 1st dyna_mem returned |
| `[12]` | Trampoline `stw 0, 48(31)` before `bctrl` | `0xCC` | r31 loaded with canary ptr, trampoline OK |
| `[13]` | Compiled PPC code before 2nd+ `bctrl` | `0xEE` | Subsequent genCallDynaMem reached |

### Files
- `ppc_dynarec.c`: `dyna_canary[16]` global, C-code stores [0]/[5], `dyna_mem()` stores [3]/[4], trampoline `ld` of r31 and store [12], print in `dynarec()` loop
- `MIPS-to-PPC.c`: `genCallDynaMem()` emits compiled stores for [10]/[11] (1st call) and [13] (subseq calls) using `mem_call_seq` counter

### SIGALRM timeout + canary diagnostics (Jun 10)

Since the CANARY line is only printed AFTER `dyna_run()` returns (it hangs before the print), added a SIGALRM timeout mechanism:

1. **`#include <signal.h>`, `#include <unistd.h>`** — for `signal()`/`alarm()`
2. **`dyna_alarm_handler()`** — SIGALRM handler: prints full canary dump and calls `_exit(1)`
3. **PRE-RUN CANARY print** — before each `dyna_run()`, prints the canary state left by the previous run
4. **Canary reset** — `memset` to zero before each `dyna_run()` call
5. **Alarm around `dyna_run()`** — `alarm(5)` before, `alarm(0)` + `signal(SIGALRM, SIG_DFL)` after

When the first `dyna_run()` hangs, after 5 seconds the handler fires and shows where it stuck.
[12]=0xCC
[10]=0xCC
[11]=0xDD
[3]=1
[4]=1
[10]=0x00 + [12]=0xCC hang before first memory access

key file ppc_dynarec.c:271-302 alarm + canary diagnostic wrapping dyna_run().


### Reading the CANARY line
```
CANARY [0]=1 [3]=0 [4]=0 [5]=0 [10]=0x00 [11]=0x00 [12]=0xCC
```
- `[12]=0xCC` + `[0]=1` + `[5]=0`: trampoline ran, `bctrl` never returned → hang in compiled code
  - `[10]=0xCC`: bctrl for 1st dyna_mem is about to be called but never returns
  - `[10]=0x00`: code hung before reaching the first memory access (earlier instruction)
- `[3]=1` + `[4]=0`: dyna_mem entered but hung inside the switch
- `[11]=0xDD` + `[5]=0`: dyna_mem returned but asm block never completed (BNELR or subsequent call hung)
- `[13]=0xEE`: at least two memory accesses reached in compiled code

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
