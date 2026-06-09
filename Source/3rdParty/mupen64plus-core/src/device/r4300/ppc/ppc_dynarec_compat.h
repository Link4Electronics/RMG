#ifndef PPC_DYNAREC_COMPAT_H
#define PPC_DYNAREC_COMPAT_H

#include <stdint.h>
#include "device/r4300/r4300_core.h"
#include "device/r4300/tlb.h"
#include "device/r4300/interrupt.h"
#include "device/memory/memory.h"
#include "main/main.h"
#include "api/callbacks.h"
#include "api/m64p_types.h"

#include "Recompile.h"
#include "Wrappers.h"
#include "Recomp-Cache.h"

extern struct r4300_core* ppc_dynarec_r4300;

/* Old-style global register arrays */
extern long long int reg[36];
extern uint32_t reg_cop0[32];
extern double *reg_cop1_double[32];
extern float *reg_cop1_simple[32];
extern long long int reg_cop1_fgr_64[32];
extern uint32_t FCR0, FCR31;
extern uint32_t last_addr, interp_addr;
extern uint32_t next_interupt, CIC_Chip;
extern int llbit;
extern uint32_t delay_slot;

#define hi (reg[32])
#define lo (reg[33])
#define local_rs (reg[34])
#define local_rt (reg[35])

/* Profiling stubs */
#define start_section(a)
#define end_section(a)
#define refresh_stat()

/* PowerPC block globals (from Recompile.h) */
extern PowerPC_block *blocks[0x100000];
extern PowerPC_block *actual;
extern char invalid_code[0x100000];

/* Interrupt handler stub using RMG API */
static inline void gen_interupt(void) {
    if (ppc_dynarec_r4300)
        gen_interrupt(ppc_dynarec_r4300);
}

/* Memory access compatibility functions */
static inline unsigned long get_physical_addr(unsigned int vaddr) {
    uint32_t paddr;
    if (r4300_read_aligned_word(ppc_dynarec_r4300, vaddr, &paddr))
        return paddr & 0x1FFFFFFF;
    return 0xFFFFFFFF;
}

/* do_SP_Task stub */
static inline void do_SP_Task(int delayedDP, int cycles) {
    (void)delayedDP; (void)cycles;
}

/* jump_to stub - used by recompiled code return */
static inline void jump_to_func(void) { }
static inline void dyna_jump(void) { }

#endif
