# Project context

## Target platform

- **Architecture:** powerpc64 big-endian (PPC64 BE)
- **ABI:** ELFv2
- **OS:** Power Linux kernel 7.0.11
- **Page size:** 64 KB
- **Sub-arch:** Power Mac G4/G5 (AltiVec)
- **Dev machine:** x86_64 (cross-compilation)

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

**Source:** `/run/media/orestes/Guinea_pig/Projetos/mupen64-360/`  
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
