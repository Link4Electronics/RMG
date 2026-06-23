# Project context

## Target platform

- **Architecture:** powerpc64 big-endian (PPC64 BE)
- **ABI:** ELFv2
- **OS:** Power Linux kernel 7.0.11
- **Page size:** 64 KB
- **Sub-arch:** Power Mac G4/G5 (AltiVec)
- **Dev machine:** x86_64 (has the repo cloned to make changes)
- **Workflow:** edit files on dev machine → user compiles on G5 → user reports issues

## Project overview

**RMG** (Rosalie's Mupen GUI, v0.8.9) — N64 emulator frontend combining:
- `RMG` — Qt6 GUI
- `RMG-Core` — C++ wrappers around mupen64plus-core C API
- `mupen64plus-core` — N64 emulator engine (R4300i MIPS III CPU)
- Video/audio/input/RSP plugins (GLideN64, paraLLEl-RDP, HLE RSP, etc.)

---

# NEW_DYNAREC PPC64 backend

## Reference architecture: ARM64 backend

**Source:** `device/r4300/new_dynarec/arm64/assem_arm64.c`
The PPC64 backend is a port of the ARM64 backend, using the same
`new_dynarec.c` generic engine with PPC64-specific assembly emitters.

## File inventory (`device/r4300/new_dynarec/ppc64/`)

| File | Lines | Role |
|------|-------|------|
| `assem_ppc64.c` | ~3057 | PPC64 instruction emitters, arch_init(), float/fconv/fcomp_assemble, TLB, cache |
| `assem_ppc64.h` | 44 | Register model, ABI constants, feature flags |
| `linkage_ppc64.S` | 397 | ELFv2 trampolines: jump_vaddr, verify_code, cc_interrupt, dyna_start/stop |

## Register model (`assem_ppc64.h`)

| PPC register | Role |
|-------------|------|
| r1 | Stack pointer (excluded from alloc) |
| r2 | TOC pointer (excluded) |
| r13 | Thread pointer (excluded) |
| r14 | HOST_CCREG — cycle count |
| r15 | HOST_BTREG — base address register |
| r31 | FP — frame pointer to hot_state |
| r0, r11, r12 | Scratch (caller-save) |
| r3–r10 | Arguments (caller-save) |
| r16–r30 | General purpose (callee-save) |

## Build system

### CMake auto-detection (`CMakeLists.txt:16-55`)

Three methods, used in order:

1. **CMAKE_SYSTEM_PROCESSOR** — matches `^(powerpc|ppc)64$` (native builds)
2. **CMAKE_C_COMPILER_TARGET** — matches `powerpc64|ppc64` (cross-compile toolchains)
3. **check_c_source_compiles** — checks `__powerpc64__`, `__ppc64__`, or `_ARCH_PPC64` **and** `!__LITTLE_ENDIAN__` (reliable for both native and cross)

Sets `POWERPC64_DYNAREC=ON` when detected. Use `cmake -DPOWERPC64_DYNAREC=OFF` to override.

### Makefile integration

In `Source/3rdParty/CMakeLists.txt`:
- `DYNAREC=ppc64` passed to core Makefile when `POWERPC64_DYNAREC=ON`
- `HOST_CPU=powerpc64 BIG_ENDIAN=1` passed to all plugin Makefiles
- `NO_ASM=0` for PPC targets (we need assembly for linkage_ppc64.S)
- rsp-parallel (SSE2) disabled when `POWERPC64_DYNAREC=ON`
- `POWERPC64_DYNAREC` → `NEW_DYNAREC=5` in Makefile

---

# Plugin compatibility (PPC64 BE)

| Plugin | Build | Runtime | Notes |
|--------|-------|---------|-------|
| `mupen64plus-core` | **OK** | **OK** | Interpreter works; PPC64 NEW_DYNAREC via cmake `-DPOWERPC64_DYNAREC=ON` |
| `mupen64plus-rsp-hle` | **OK** | **OK** | Endian-aware via `M64P_BIG_ENDIAN` memory macros |
| `mupen64plus-rsp-cxd4` | **OK** (scalar) | **OK** | SSE2 auto-disabled on PPC |
| `mupen64plus-video-GLideN64` | **OK** | **IN PROGRESS** | SM64 boots, black screen status. CI4/CI8/CI16 palette fixed. All XOR patterns patched. |
| `mupen64plus-video-rice` | **OK** | **IN PROGRESS** | Flicker-free. SM64 CI16→RGBA conversion bug on BE. z-ordering inverted. |
| `mupen64plus-video-parallel` | **OK** | **N/A** | No Vulkan on G5 |
| `mupen64plus-input-raphnetraw` | **OK** | No arch-specific code |
| `RMG-Audio` | **OK** | `SDL_AUDIO_S16` auto-detects host endian |
| `mupen64plus-rsp-parallel` | **BLOCKED** | **N/A** | Unconditional SSE2 — disabled |

# GLideN64 endian issues (PPC64 BE)

## RDRAM byte order

mupen64plus-core stores RDRAM in **host byte order**:
- x86 LE: N64 BE data written to RDRAM stored as LE words
- PPC64 BE: N64 BE data stored as BE words (native byte order)

`*(u32*)&RDRAM[addr]` produces the correct N64 value regardless of endianness. However, 16-bit and 8-bit accesses differ between LE and BE because of byte-reversed storage within each 32-bit word.

## LE-specific XOR patterns
GLideN64 uses ad-hoc XOR patterns on pointers/indexes to compensate for LE byte-reversed storage. These must compile out on BE.

---


# PPC dynarec status

## NEW_DYNAREC backend (current)

### Infrastructure — COMPLETE

| Component | Status |
|-----------|--------|
| Code buffer allocation | **OK** (mmap RWX — uses `mmap(NULL, ..., PROT_READ\|PROT_WRITE\|PROT_EXEC)` because `mprotect` on `extra_memory` fails with PPC64's 64KB pages vs 4KB struct alignment) |
| Linkage trampolines | **OK** (linkage_ppc64.S + cc_interrupt, verify_code) |
| Register allocator | **OK** (generic new_dynarec.c + host_reg_ppc[]) |
| save_regs/restore_regs | **OK** |
| emit_64bit_call | **OK** (via r12 + mtctr + bctrl) |
| emit_call | **OK** — calls emit_64bit_call (not `bl`) |
| genUpdateCount | **OK** |
| TLB stubs (do_tlb_r/w) | **OK** |
| MINIHT (do_miniht_*) | **OK** |
| literal_pool | **OK** |
| dcbf/icbi cache flush | **OK** |
| set_jump_target | **OK** (B-case uses 24-bit mask `0xFFFFFF`, BC uses 14-bit `0x3fff`) |
| emit_extjump2 | **OK** (loads target into ARG1_REG, calls `dynamic_linker` — not computed address) |
| alloc_reg64 | **OK** (allocates both `tr` and `tr\|64` in separate regmap slots) |

### Runtime bugs found and fixed (in order)

| Bug | Symptom | Fix |
|-----|---------|-----|
| `@h`/`@l` on 16-bit values | `new_recompile_block` called with `addr=0xA440` | Hardcode `lis`/`ori` for 0xA4000040 |
| `EXCLUDE_REG = 1` | `get_reg` skips host index 1; `alloc_reg` uses it → assertion | `EXCLUDE_REG = HOST_REGS` (28) |
| `alloc_reg64` missing `tr\|64` | `get_reg(regmap, rs1[i]\|64)` returns -1 → assertion | Allocate both halves |
| `set_jump_target` B-case 26-bit mask | Corrupted opcode for negative offsets | `0x3ffffff` → `0xFFFFFF` |
| `do_miniht_insert` `add_to_linker` on MOVIMM64 | `set_jump_target` asserts on non-branch insn | Remove `add_to_linker` call |
| Code buffer not executable | Segfault at `g_dev` + offset (mprotect fails on 64KB pages) | Use `mmap(MAP_ANONYMOUS, RWX)` instead |
| `emit_extjump2` computed address instead of calling linker | Segfault at mmap base+0x10 | Rewrite to load target+r3 → `emit_jmp(linker)` (match ARM64 pattern) |
| Missing `emit_addimm64` | Compile error: implicit declaration | Add `emit_addimm64(rs, imm, rt)` delegating to `emit_addimm` |
| Missing `emit_addimm_and_set_flags` | Compile error: implicit declaration | Add 2-instruction sequence (`addi` + `cmpwi`) for CR0 |
| `emit_movimm64` wrong 64-bit constant construction | Segfault at code buffer+0x10 on first block | Rewrite to build upper 32 bits → `sldi` by 32 → lower 32 bits (proper PPC64 pattern) |
| `emit_jeq`/`emit_jne`/etc BO values swapped | Game data never loaded — BNE inverted → RI_SELECT check always fell through | Revert: `jeq→12`, `jne→4`, `jl→12`, `jge→4`, `jb→12`, `jae→4`, `js→12`, `jns→4`, `jno→4` (all `bt`=BO12, `bf`=BO4) |

### FPU support — C fallbacks (PPC scalar FPU not used)

| Function | Handler | Ops covered |
|----------|---------|-------------|
| `fconv_assemble_ppc64` | C fallback via named funcs | cvt_s/d_w/l, cvt_d/s, cvt_w/l_s/d, rounding ops |
| `float_assemble` | C fallback via named funcs | add/sub/mul/div/sqrt/abs/mov/neg (single & double) |
| `fcomp_assemble` | C fallback via named funcs | c_f/un/eq/ueq/olt/ult/ole/ule/sf/ngle/seq/ngl/lt/nge/le/ngt |

No NEON or native PPC FPU instructions are used — AltiVec not used (the FPU C fallbacks are architecture-agnostic). Native PPC scalar FP assembly (fadd, fmul, etc.) is a future optimization.

### FPU bug notes (ARM64 originals fixed in PPC64)

- ARM64's C fallback for `cvt_d_w` and `cvt_d_s` has **ARG1_REG overwrite** bug (fcr31 pointer clobbered by source pointer). **Fixed** in PPC64 version — uses distinct ARGn_REGs.
- `float_assemble` C fallback for `abs_s`/`neg_s` in ARM64 also clobbers fcr31. **Fixed** — separate args for fcr31, source, dest.
- `mov_s`/`mov_d` correctly skip fcr31 (2-arg functions).

### Not yet written (low priority)

All emitters are implemented. Future optimizations:

| Feature | Fallback |
|---------|----------|
| emit_prefetchreg | Implemented (dcbt) |
| emit_cmov_* variants | Uses bc+skip pattern |
| conditional moves within generated code | Uses bc+skip pattern |
| Native PPC scalar FPU (fadd/fmul/fdiv) | C fallbacks (working) |

---

# NEW_DYNAREC PPC64 diagnostics

The PPC64 backend has built-in stderr diagnostics (added 2024-06):
- `arch_init()` prints buffer addresses
- `new_recompile_block()` prints block addresses and first-block hex dump
- Controlled by `#if NEW_DYNAREC == NEW_DYNAREC_PPC64` guard in `new_dynarec.c`

---

# Estimated completion: ~45%

## Code structure: ~98%
All required functions exist and compile clean (0 errors, 0 warnings). The file is structurally complete at ~3057 lines (ARM64 reference is 4661 — PPC64 is shorter because FPU uses C fallbacks instead of NEON SIMD assembly).

## Runtime correctness: ~10%
The generated code was **incorrect** because all branch emitters had inverted BO values (commit 8958b913 swapped BO=12↔BO=4). SM64 booted to black screen — BNE at the RI_SELECT check always fell through, so IPL3 never loaded the game. After reverting BO values to the original correct mapping, the dynarec should now follow the correct code paths.

## Breakdown by subsystem

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Branch encoding (B/BC/BL) | **FIXED** | BO=12 for `bt` (branch-true), BO=4 for `bf` (branch-false). Commit 8958b913 wrongly swapped them; now reverted to original correct mapping. |
| ALU (add/sub/and/or/xor) | **Likely OK** | `_HR` macros applied consistently |
| Memory load/store (D-form) | **Likely OK** | Uses `D_FORM_HR`, FP offsets within range |
| 64-bit constant loading | **FIXED** | Was broken (missing `sldi`), now correct |
| Register allocator | **Likely OK** | Same as other archs, tested |
| TLB / memory map | **Untested** | Need to verify mapping logic on BE |
| Mini-HT (hash table jump) | **Untested** | May have reg-index vs PPC-num confusion |
| FPU C fallbacks | **Untested** | Only triggers if game uses FPU early |
| Cycle counting / cc_interrupt | **Untested** | Depends on `emit_addimm_and_set_flags` CR0 behavior |
| `jump_vaddr_reg[]` trampolines | **Untested** | Indexed by PPC register number |
| Linkage assembly | **Likely OK** | Working — first block entered successfully |

## Next debugging steps

1. **Run with diagnostics:** `./rmg 2> rmg.log` and examine the first block's compiled PPC instruction hex dump
2. **Compare with expected output:** Manually decode the first few MIPS → PPC translations and verify correctness
3. **Test with `RECOMPILER_DEBUG`:** Enables verbose `assem_debug` logging throughout `new_dynarec.c`
4. **Isolate first incorrect block:** Narrow down which MIPS instruction produces wrong PPC code by removing blocks from the log

1. **Run with diagnostics:** `./rmg 2> rmg.log` and examine the first block's compiled PPC instruction hex dump
2. **Compare with expected output:** Manually decode the first few MIPS → PPC translations and verify correctness
3. **Test with `RECOMPILER_DEBUG`:** Enables verbose `assem_debug` logging throughout `new_dynarec.c`
4. **Isolate first incorrect block:** Narrow down which MIPS instruction produces wrong PPC code by removing blocks from the log

---

# Old PPC64 Dynarec
Removed on commit ab38c35a, contained some things that may or may not worth porting to PPC64 new_dynarec

Fastmem direct KSEG0→RDRAM — old code has RLWINM(rd,base,0,2,31); ORIS(rd,rd,0x4000) for inlined loads from KSEG0, avoiding C fallback for every memory access.
FPU native PPC instructions — old code uses fctiwz, mtfsfi, fcmpu, stfiwx directly instead of C fallbacks. Currently our backend uses C for everything FPU.
Interrupt clamping — PPCD_CHECK_INTERVAL forces periodic returns to the dispatcher, preventing infinite loops in compiled code.
decodeNInterpret() — ~500-line C interpreter for un-compilable instructions. Could serve as gen_interpreter() fallback.
Per-block jump resolution — the RecompCache_Link approach backpatches 5+ instructions for far targets (needed when 24-bit B-range is insufficient).

---

# Relevant files

| File | Path |
|------|------|
| PPC64 emitters | `device/r4300/new_dynarec/ppc64/assem_ppc64.c` |
| PPC64 header | `device/r4300/new_dynarec/ppc64/assem_ppc64.h` |
| Linkage asm | `device/r4300/new_dynarec/ppc64/linkage_ppc64.S` |
| Shared engine | `device/r4300/new_dynarec/new_dynarec.c` |
| ARM64 reference | `device/r4300/new_dynarec/arm64/assem_arm64.c` |
| CMake root | `CMakeLists.txt` |
| 3rdParty CMake | `Source/3rdParty/CMakeLists.txt` |
| Makefile | `projects/unix/Makefile` |

---
