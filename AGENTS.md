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

**Fix:** Changed `bl 4` to `bl .+4` (standard GAS idiom). Also merged two separate asm blocks into one. `ppc_dynarec.c`.

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

### Bug: D-cache/I-cache coherency (FIXED)

**Symptom:** `dcbf` is a hint instruction (can be ignored); `ICInvalidateRange` had no `sync` before `isync`.

**Fix:** Changed `dcbf` → `dcbst` (guaranteed store-back); added `sync` before `isync`. `Recomp-Cache.c`.

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

### Bug: Backward branch BLR safety net (FIXED)

**Symptom:** Backward conditional branches could loop within the block without ever returning to the dispatcher if the Count check somehow fails (e.g., stale `next_interupt` global).

**Fix:** At `MIPS-to-PPC.c:219-222`, for `offset < 0`: emit `LD r0, DYNAOFF_LR(1); MTLR r0; BLR` after the BC(nbo) branch. This guarantees every taken backward branch returns to the dispatcher, preventing infinite internal loops.

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

### Known issues

1. **Stale `next_interupt` global**: After `gen_interrupt()` modifies `r4300->cp0.next_interrupt`, the global `next_interupt` (that r21 points to in recompiled code) is never synced back. The dispatcher at `ppc_dynarec.c:243` also reads the stale global. After the first interrupt fires, Count checks in recompiled blocks use the wrong comparison value.
2. **Floating-point control** — `fesetround()` / `mtfsf` / `mffs` path needs runtime verification that rounding mode is set correctly for N64 FE_TOWARDZERO emulation.
3. **No VMX128 in the CPU dynarec** — The recompiler emits only scalar PPC (add, lwz, stw, rlwinm, etc.). LVX/STVX/VOR macros are standard AltiVec and work on both G4/G5 and Xenon.

---

## Build system

Pass `-DPPC_DYNAREC=ON` to cmake. The `3rdParty/CMakeLists.txt` propagates:
- `PPC_DYNAREC=$<BOOL:${PPC_DYNAREC}>` to core Makefile
- `HOST_CPU=powerpc64 BIG_ENDIAN=1` (via `PPC_MAKE_FLAGS`) to all Makefile-based plugins when cross-compiling
- `override NO_ASM := 1` forced for all PPC targets (no assembly source available)

## Plugin compatibility (PPC64 BE)

| Plugin | Build | Runtime | Notes |
|--------|-------|---------|-------|
| `mupen64plus-core` | **OK** | **OK** | PPC_DYNAREC enabled via cmake option |
| `mupen64plus-rsp-hle` | **OK** | **OK** | Endian-aware via `M64P_BIG_ENDIAN` memory macros |
| `mupen64plus-rsp-cxd4` | **OK** (scalar) | **OK** | SSE2 path auto-disabled; scalar fallback performs all ops |
| `mupen64plus-video-GLideN64` | **OK** | **OK** | OpenGL-based; `xxHash` has PPC64 BE detection |
| `mupen64plus-video-parallel` | **OK** | **N/A** (no Vulkan on G5) | SSE2 guarded with `#ifdef __SSE2__`, has scalar fallback |
| `mupen64plus-input-raphnetraw` | **OK** | **OK** | No arch-specific code |
| `RMG-Audio` | **OK** | **OK** | Endian-aware via `SDL_BYTEORDER` |
| `mupen64plus-rsp-parallel` | **BLOCKED** | **N/A** | Unconditional SSE2 in `rsp_core.cpp` — disabled when `PPC_DYNAREC=ON` |

### Makefile warning fixes
Changed `$(warning ...)` to `$(info ...)` with "supported by RMG" for PPC blocks in:
- `mupen64plus-core/projects/unix/Makefile` (3 PPC blocks)
- `mupen64plus-rsp-hle/projects/unix/Makefile` (2 PPC blocks)
- `mupen64plus-rsp-cxd4/projects/unix/Makefile` (2 PPC blocks)
