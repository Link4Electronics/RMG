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

## Dynarec reference: mupen64-360

**Source:** `https://github.com/gligli/mupen64-360`  
**Origin:** Wii64 → mupen64-360 (Xbox 360 / Xenon PPC port)

### Key files (PPC dynarec)

| File | Role |
|------|------|
| `source/r4300/ppc/MIPS-to-PPC.c` | MIPS→PPC instruction translator (~4000 lines) |
| `source/r4300/ppc/Recompile.c` | Block compilation, caching, execution |
| `source/r4300/ppc/Register-Cache.c` | GPR/FPR register allocator |
| `source/r4300/ppc/Wrappers.c` | `dynarec()` entry point, `dyna_run()` trampoline |
| `source/r4300/ppc/PowerPC.h` | PPC instruction encoding macros (~200 opcodes) |
| `source/r4300/ppc/MIPS.h` | MIPS opcode decoding helpers |
| `source/r4300/ppc/FuncTree.c` | BST for recompiled block lookup |
| `source/r4300/ppc/Recomp-Cache.c` | Recompiled code cache with LRU |
| `source/r4300/ppc/disasm/` | Full PPC + AltiVec disassembler |

### Key findings

1. **No VMX128 in the CPU dynarec** — The recompiler emits only scalar PPC (add, lwz, stw, rlwinm, etc.). The LVX/STVX/VOR macros in `Recompile.h` are standard AltiVec and unused.
2. **Xenon-specific code** only in: cache management (`memdcbf`, `memicbi`, `DCFlushRange`, `ICInvalidateRange`), heap (`__lwp_heap_*` from libogc), and platform headers (`<ppc/cache.h>`, `<debug.h>`, `<xetypes.h>`).
3. **dyna_run() trampoline** uses standard PPC 32-bit ABI (r14-r23 saved regs, standard stack frame) — no Xenon-specific asm.
4. **FPU handling** (`fpu.h`) uses Xenon SPE instructions (`mfspefscr`/`mtspefscr`) — must be replaced with standard `mtfsf`/`mffs` or `<fenv.h>`.

### Porting to standard PPC Linux

Replace:
- `#include <ppc/cache.h>` + `memdcbf()`/`memicbi()` → inline `dcbf`/`icbi` asm or `__builtin___clear_cache()`
- `#include <debug.h>` → drop or use `printf`
- `#include <ppc/timebase.h>` → `mftb()` via inline asm
- `#include <xetypes.h>` → `<stdint.h>` with `uint32_t`
- `__lwp_heap_*` → `malloc()`/`free()` + manual LRU
- `section(".bss.beginning.upper")` → `__attribute__((aligned(65536)))`
- `mfspefscr`/`mtspefscr` → `mffs`/`mtfsf` or `<fenv.h>` `fesetround()`
- Compile with `-mcpu=G4` or `-mcpu=G5 -maltivec` instead of `-mcpu=cell`
- Link as standard `.so` instead of Xenon ELF binary

### Integration into mupen64plus-core

The mupen64-360 dynarec does **not** match the old dynarec interface (`recomp.c` + `dynarec.c` + `assemble.c` + `regcache.c`). Two approaches:

**A. Standalone (`PPC_DYNAREC` path)** — Add as a new backend alongside existing ones with `#ifdef PPC_DYNAREC` in `r4300_core.h`/`r4300_core.c`, using the mupen64-360's own block dispatch.

**B. New Dynarec backend** — Could also target `new_dynarec/` but would require a PPC assembly backend (`assem_ppc.c` + `linkage_ppc.S`) matching Ari64's architecture — this is a much larger effort.

The **A. Standalone path** is more practical: the mupen64-360 code already has its own complete dispatch loop, block cache, and translator that can replace the entire r4300 execution path when `EMUMODE_DYNAREC` is selected.

## AltiVec strategy

Since the existing mupen64-360 dynarec doesn't use any vector instructions, adding AltiVec would be a **new feature** (no risk of breaking VMX128). Targets:

1. **Vectorized memory fill/copy** for RDRAM operations
2. **Parallel flag computation** for MIPS condition codes
3. **Vectorized COP1 (FPU)** operations using AltiVec

The existing `EMIT_LVX`/`EMIT_STVX`/`EMIT_VOR` macros in `Recompile.h` are standard AltiVec and work on both G4/G5 and Xenon without modification.

## PPC dynarec integration status

All 11 PPC dynarec source files created in `device/r4300/ppc/` (from mupen64-360, adapted for RMG):

| File | Lines | Role |
|------|-------|------|
| `MIPS-to-PPC.c` | 2098+ | MIPS→PPC instruction translator (+ `emit_64bit_call`) |
| `PowerPC.h` | 1156 | PPC instruction encoding macros (~200 opcodes) |
| `ppc_dynarec.c` | 558 | Main entry: `dynarec()` loop, `decodeNInterpret()`, `dyna_mem()`, trampoline |
| `Recompile.h` | 318 | EMIT_* macros, block compilation interface |
| `MIPS.h` | 279 | MIPS opcode decoding helpers |
| `Recomp-Cache.c` | 256 | Recompiled code cache with LRU |
| `Recompile.c` | 382 | Block compilation, jump fixup, `genJumpPad()` LR restore fix |
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

### Critical PPC64 type width fixes

PPC64 has 8-byte `long` and `unsigned long`. The recompiled PPC code uses 32-bit `lwz`/`stw` to access these globals, requiring exact 4-byte alignment:

| Global | Original type | Fixed type | Reason |
|--------|-------------|------------|--------|
| `reg_cop0[32]` | `unsigned long` | `uint32_t` | Accessed via `lwz` at offset `rd*4` |
| `FCR0`, `FCR31` | `long` | `uint32_t` | Accessed via `lwz` at r18+0 |
| `last_addr`, `interp_addr` | `unsigned long` | `uint32_t` | Accessed via `lwz` at r20+0 |
| `delay_slot` | `unsigned long` | `uint32_t` | Accessed via embedded address |

`reg[36]` stays `long long` (8 bytes) — correct, as recompiled code accesses low 32 bits at offset `i*8+4` on big-endian.

### PPC64 bugs found during bringup

| Bug | Fix | File | Lines |
|-----|-----|------|-------|
| `get_physical_addr()` returned loaded data values instead of translated addresses | Added proper TLB address translation path | `ppc_dynarec.c` | 533-544 |
| `genJumpPad()` emitted `BLR` without restoring LR | Added `LD r0, DYNAOFF_LR(r1)` + `MTLR r0` before `BLR` | `Recompile.c` | 173-174 |
| `lwz` used for LR restore (32-bit on PPC64) | Changed to `ld` (64-bit) | `MIPS-to-PPC.c` | 1095, 1912 |
| **`bl` ±32 MB range exceeded**: mmap'd code buffer can be 575+ MB from library text segment; all 14 CALL jumps silently overflow 24-bit LI field and jump to garbage | Replaced all `EMIT_B(add_jump(..., 1, 1), 0, 1)` with `emit_64bit_call()` — loads full 64-bit address into r12, `mtctr` + `bctrl` for unlimited range | `MIPS-to-PPC.c` | `emit_64bit_call()` at line ~37, 14 call sites across file |
| **FAILSAFE_REC_NO_VM not set**: fast memory path (`genCallDynaMemVM` lines 1947-2073) emits direct PPC loads/stores at N64 KSEG1 addresses `(addr & 0x1FFFFFFF) \| 0x40000000` — unmapped on Linux | Set `failsafeRec \|= FAILSAFE_REC_NO_VM` in `ppc_dynarec_init()` to force slow path via `dyna_mem()` C function | `ppc_dynarec.c` | 488 |
| **Register allocator uses r2/r13 on PPC64 ELFv2**: r2=TOC pointer, r13=thread pointer/small data area. If allocator maps a MIPS GPR to r2/r13, all C function calls from recompiled code have corrupted TOC/DSA. | Set `availableRegsDefault[2]=0` and `availableRegsDefault[13]=0` in `Register-Cache.c` to reserve them. Only r24-r31 are now available for MIPS GPR mapping. | `Register-Cache.c` | 13-17 |
| **`bl 4` trampoline wrong on Linux GAS**: Xenon assembler interprets `bl 4` as `bl .+4` (skip 1 instruction), but Linux GAS interprets it as `bl` to absolute address 4 — huge negative offset, skips `mtctr`, so `bctrl` hits uninitialized CTR (32-bit truncated address → crash at `0x00000000CC076770`). | Changed `bl 4` to `bl .+4` (standard GAS idiom). Also merged two separate asm blocks into one to prevent compiler from corrupting DYNAREG (r14-r23) between blocks. | `ppc_dynarec.c` | 97-136 |
| **D-cache/I-cache coherency**: `dcbf` is a hint instruction (can be ignored); `ICInvalidateRange` had no `sync` before `isync`. | Changed `dcbf` → `dcbst` (guaranteed store-back to memory); added `sync` before `isync` in `ICInvalidateRange`. | `Recomp-Cache.c` | 13-29 |

The `emit_64bit_call()` helper uses r12 (consistent with PPC64 ELFv2 ABI for function entry point):
```c
static void emit_64bit_call(uintptr_t target) {
    PowerPC_instr tmp;
    uint64_t t = (uint64_t)target;
    EMIT_LIS(12, (t >> 48) & 0xFFFF);
    EMIT_ORI(12, 12, (t >> 32) & 0xFFFF);
    GEN_RLDICR(tmp, 12, 12, 32, 31, 0); set_next_dst(tmp);
    EMIT_ORIS(12, 12, (t >> 16) & 0xFFFF);
    EMIT_ORI(12, 12, t & 0xFFFF);
    EMIT_MTCTR(12);
    EMIT_BCTRL(0);
}
```

### Build system

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
