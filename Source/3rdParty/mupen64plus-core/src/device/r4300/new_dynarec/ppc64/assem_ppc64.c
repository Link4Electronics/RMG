/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - assem_ppc64.c                                            *
 *   Copyright (C) 2009-2018 Gillou68310                                    *
 *   Copyright (C) 2024 RMG PPC64 dynarec port                              *
 *                                                                          *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                          *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                          *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/* FP offsets from r31 (frame pointer = &new_dynarec_hot_state) */
#define fp_cycle_count         (offsetof(struct new_dynarec_hot_state, cycle_count))
#define fp_invc_ptr            (offsetof(struct new_dynarec_hot_state, invc_ptr))
#define fp_fcr31               (offsetof(struct new_dynarec_hot_state, cp1_fcr31))
#define fp_regs                (offsetof(struct new_dynarec_hot_state, regs))
#define fp_hi                  (offsetof(struct new_dynarec_hot_state, hi))
#define fp_lo                  (offsetof(struct new_dynarec_hot_state, lo))
#define fp_cp0_regs(x)         ((offsetof(struct new_dynarec_hot_state, cp0_regs)) + (x)*sizeof(uint32_t))
#define fp_rounding_modes      (offsetof(struct new_dynarec_hot_state, rounding_modes))
#define fp_fake_pc             (offsetof(struct new_dynarec_hot_state, fake_pc))
#define fp_ram_offset          (offsetof(struct new_dynarec_hot_state, ram_offset))
#define fp_mini_ht             (offsetof(struct new_dynarec_hot_state, mini_ht))
#define fp_memory_map          (offsetof(struct new_dynarec_hot_state, memory_map))

/* Extern assembly trampolines */
extern void jump_vaddr_r3(void);
extern void jump_vaddr_r4(void);
extern void jump_vaddr_r5(void);
extern void jump_vaddr_r6(void);
extern void jump_vaddr_r7(void);
extern void jump_vaddr_r8(void);
extern void jump_vaddr_r9(void);
extern void jump_vaddr_r10(void);
extern void jump_vaddr_r11(void);
extern void jump_vaddr_r12(void);
extern void jump_vaddr_r14(void);
extern void jump_vaddr_r16(void);
extern void jump_vaddr_r17(void);
extern void jump_vaddr_r18(void);
extern void jump_vaddr_r19(void);
extern void jump_vaddr_r20(void);
extern void jump_vaddr_r21(void);
extern void jump_vaddr_r22(void);
extern void jump_vaddr_r23(void);
extern void jump_vaddr_r24(void);
extern void jump_vaddr_r25(void);
extern void jump_vaddr_r26(void);
extern void jump_vaddr_r27(void);
extern void jump_vaddr_r28(void);
extern void jump_vaddr_r29(void);
extern void jump_vaddr_r30(void);
extern void dyna_linker(void);
extern void dyna_linker_ds(void);
extern void verify_code(void);
extern void cc_interrupt(void);
extern void fp_exception(void);
extern void jump_syscall(void);
extern void jump_eret(void);
extern void do_interrupt(void);
extern void breakpoint(void);
static void emit_cmp(int rs, int rt);
static void emit_cmpu(int rs, int rt);
static void emit_cmpimm(int rs, int imm);
static void emit_cmpuimm(int rs, int imm);
static void emit_readword_indexed(int offset, int rs, int rt);
static void emit_writeword_indexed(int rs, int offset, int ra);
static void invalidate_addr(u_int addr);
static void emit_storereg(int r, int hr);
static void emit_64bit_call(intptr_t addr, int scratch);
static void emit_jmp(intptr_t a);

static uintptr_t literals[1024][2];
static unsigned int needs_clear_cache[1 << (TARGET_SIZE_2 - 17)];

/* Indexed by HOST INDEX (0-26), not PPC register number.
   new_dynarec.c passes host indices to jump_vaddr_reg[rs]. */
static const uintptr_t jump_vaddr_reg[27] = {
    (intptr_t)jump_vaddr_r3,        /* host  0 → PPC r3 */
    (intptr_t)jump_vaddr_r4,        /* host  1 → PPC r4 */
    (intptr_t)jump_vaddr_r5,        /* host  2 → PPC r5 */
    (intptr_t)jump_vaddr_r6,        /* host  3 → PPC r6 */
    (intptr_t)jump_vaddr_r7,        /* host  4 → PPC r7 */
    (intptr_t)jump_vaddr_r8,        /* host  5 → PPC r8 */
    (intptr_t)jump_vaddr_r9,        /* host  6 → PPC r9 */
    (intptr_t)jump_vaddr_r10,       /* host  7 → PPC r10 */
    (intptr_t)jump_vaddr_r11,       /* host  8 → PPC r11 */
    (intptr_t)jump_vaddr_r12,       /* host  9 → PPC r12 (HOST_TEMPREG) */
    (intptr_t)jump_vaddr_r14,       /* host 10 → PPC r14 (HOST_CCREG) */
    (intptr_t)breakpoint,           /* host 11 → PPC r15 (HOST_BTREG — excluded) */
    (intptr_t)jump_vaddr_r16,       /* host 12 → PPC r16 */
    (intptr_t)jump_vaddr_r17,       /* host 13 → PPC r17 */
    (intptr_t)jump_vaddr_r18,       /* host 14 → PPC r18 */
    (intptr_t)jump_vaddr_r19,       /* host 15 → PPC r19 */
    (intptr_t)jump_vaddr_r20,       /* host 16 → PPC r20 */
    (intptr_t)jump_vaddr_r21,       /* host 17 → PPC r21 */
    (intptr_t)jump_vaddr_r22,       /* host 18 → PPC r22 */
    (intptr_t)jump_vaddr_r23,       /* host 19 → PPC r23 */
    (intptr_t)jump_vaddr_r24,       /* host 20 → PPC r24 */
    (intptr_t)jump_vaddr_r25,       /* host 21 → PPC r25 */
    (intptr_t)jump_vaddr_r26,       /* host 22 → PPC r26 */
    (intptr_t)jump_vaddr_r27,       /* host 23 → PPC r27 */
    (intptr_t)jump_vaddr_r28,       /* host 24 → PPC r28 */
    (intptr_t)jump_vaddr_r29,       /* host 25 → PPC r29 */
    (intptr_t)jump_vaddr_r30,       /* host 26 → PPC r30 */
};

static uintptr_t jump_table_symbols[] = {
    (intptr_t)NULL /* TLBR */,
    (intptr_t)NULL /* TLBP */,
    (intptr_t)NULL /* MULT */,
    (intptr_t)NULL /* MULTU */,
    (intptr_t)NULL /* DIV */,
    (intptr_t)NULL /* DIVU */,
    (intptr_t)NULL /* DMULT */,
    (intptr_t)NULL /* DMULTU */,
    (intptr_t)NULL /* DDIV */,
    (intptr_t)NULL /* DDIVU */,
    (intptr_t)invalidate_addr,
    (intptr_t)dyna_linker,
    (intptr_t)dyna_linker_ds,
    (intptr_t)verify_code,
    (intptr_t)cc_interrupt,
    (intptr_t)fp_exception,
    (intptr_t)jump_syscall,
    (intptr_t)jump_eret,
    (intptr_t)do_interrupt,
    (intptr_t)TLBWI_new,
    (intptr_t)TLBWR_new,
    (intptr_t)MFC0_new,
    (intptr_t)MTC0_new,
    (intptr_t)jump_vaddr_r3,
    (intptr_t)jump_vaddr_r4,
    (intptr_t)jump_vaddr_r5,
    (intptr_t)jump_vaddr_r6,
    (intptr_t)jump_vaddr_r7,
    (intptr_t)jump_vaddr_r8,
    (intptr_t)jump_vaddr_r9,
    (intptr_t)jump_vaddr_r10,
    (intptr_t)jump_vaddr_r11,
    (intptr_t)jump_vaddr_r12,
    (intptr_t)breakpoint,            /* r13 excluded */
    (intptr_t)jump_vaddr_r14,        /* HOST_CCREG */
    (intptr_t)breakpoint,            /* r15 = HOST_BTREG */
    (intptr_t)jump_vaddr_r16,
    (intptr_t)jump_vaddr_r17,
    (intptr_t)jump_vaddr_r18,
    (intptr_t)jump_vaddr_r19,
    (intptr_t)jump_vaddr_r20,
    (intptr_t)jump_vaddr_r21,
    (intptr_t)jump_vaddr_r22,
    (intptr_t)jump_vaddr_r23,
    (intptr_t)jump_vaddr_r24,
    (intptr_t)jump_vaddr_r25,
    (intptr_t)jump_vaddr_r26,
    (intptr_t)jump_vaddr_r27,
    (intptr_t)jump_vaddr_r28,
    (intptr_t)jump_vaddr_r29,
    (intptr_t)jump_vaddr_r30,
    (intptr_t)cvt_s_w,
    (intptr_t)cvt_d_w,
    (intptr_t)cvt_s_l,
    (intptr_t)cvt_d_l,
    (intptr_t)cvt_w_s,
    (intptr_t)cvt_w_d,
    (intptr_t)cvt_l_s,
    (intptr_t)cvt_l_d,
    (intptr_t)cvt_d_s,
    (intptr_t)cvt_s_d,
    (intptr_t)round_l_s,
    (intptr_t)round_w_s,
    (intptr_t)trunc_l_s,
    (intptr_t)trunc_w_s,
    (intptr_t)ceil_l_s,
    (intptr_t)ceil_w_s,
    (intptr_t)floor_l_s,
    (intptr_t)floor_w_s,
    (intptr_t)round_l_d,
    (intptr_t)round_w_d,
    (intptr_t)trunc_l_d,
    (intptr_t)trunc_w_d,
    (intptr_t)ceil_l_d,
    (intptr_t)ceil_w_d,
    (intptr_t)floor_l_d,
    (intptr_t)floor_w_d,
    (intptr_t)c_f_s,
    (intptr_t)c_un_s,
    (intptr_t)c_eq_s,
    (intptr_t)c_ueq_s,
    (intptr_t)c_olt_s,
    (intptr_t)c_ult_s,
    (intptr_t)c_ole_s,
    (intptr_t)c_ule_s,
    (intptr_t)c_sf_s,
    (intptr_t)c_ngle_s,
    (intptr_t)c_seq_s,
    (intptr_t)c_ngl_s,
    (intptr_t)c_lt_s,
    (intptr_t)c_nge_s,
    (intptr_t)c_le_s,
    (intptr_t)c_ngt_s,
    (intptr_t)c_f_d,
    (intptr_t)c_un_d,
    (intptr_t)c_eq_d,
    (intptr_t)c_ueq_d,
    (intptr_t)c_olt_d,
    (intptr_t)c_ult_d,
    (intptr_t)c_ole_d,
    (intptr_t)c_ule_d,
    (intptr_t)c_sf_d,
    (intptr_t)c_ngle_d,
    (intptr_t)c_seq_d,
    (intptr_t)c_ngl_d,
    (intptr_t)c_lt_d,
    (intptr_t)c_nge_d,
    (intptr_t)c_le_d,
    (intptr_t)c_ngt_d,
    (intptr_t)add_s,
    (intptr_t)sub_s,
    (intptr_t)mul_s,
    (intptr_t)div_s,
    (intptr_t)sqrt_s,
    (intptr_t)abs_s,
    (intptr_t)mov_s,
    (intptr_t)neg_s,
    (intptr_t)add_d,
    (intptr_t)sub_d,
    (intptr_t)mul_d,
    (intptr_t)div_d,
    (intptr_t)sqrt_d,
    (intptr_t)abs_d,
    (intptr_t)mov_d,
    (intptr_t)neg_d,
    (intptr_t)read_byte_new,
    (intptr_t)read_hword_new,
    (intptr_t)read_word_new,
    (intptr_t)read_dword_new,
    (intptr_t)write_byte_new,
    (intptr_t)write_hword_new,
    (intptr_t)write_word_new,
    (intptr_t)write_dword_new,
    (intptr_t)LWL_new,
    (intptr_t)LWR_new,
    (intptr_t)LDL_new,
    (intptr_t)LDR_new,
    (intptr_t)SWL_new,
    (intptr_t)SWR_new,
    (intptr_t)SDL_new,
    (intptr_t)SDR_new,
      (intptr_t)breakpoint
};

/* FPU functions are from fpu.h (included via new_dynarec.c's include chain).
 * The jump_table_symbols entries below reference them by address.
 * The compiled code sets up r3-r5 arguments and calls through the table. */

/* ======================================================================== */
/* Cache flush                                                              */
/* ======================================================================== */
static void cache_flush(char *start, char *end)
{
    uintptr_t addr;
    uintptr_t start_addr = (uintptr_t)start;
    uintptr_t end_addr   = (uintptr_t)end;
    /* PPC970: sync (drain store buffer) THEN dcbf (flush D-cache to L2),
     * THEN icbi (invalidate I-cache), then final sync+isync.
     * The dcbf MUST see the stored instructions in D-cache, hence sync first. */
    __asm__ volatile("sync" : : : "memory");
    for (addr = start_addr & ~63; addr < end_addr; addr += 32) {
        __asm__ volatile("dcbf 0,%0" : : "r"(addr) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
    for (addr = start_addr & ~63; addr < end_addr; addr += 32) {
        __asm__ volatile("icbi 0,%0" : : "r"(addr) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
    __asm__ volatile("isync" : : : "memory");
}

/* ======================================================================== */
/* Jump target fixup                                                        */
/* ======================================================================== */
static void set_jump_target(intptr_t addr, uintptr_t target)
{
    u_int *ptr = (u_int *)addr;
    if (ptr == NULL) return;
    intptr_t offset = (intptr_t)target - (intptr_t)addr;
    
    if ((*ptr & 0xFC000000) == 0x48000000) {
        /* Unconditional branch (B or BL): 24-bit LI, ±32MB */
        assert(offset >= -33554432LL && offset < 33554432LL);
        *ptr = (*ptr & 0xFC000003) | (((offset >> 2) & 0xFFFFFF) << 2);
    }
    else if ((*ptr & 0xFC000000) == 0x40000000) {
        /* Conditional branch (BC): 14-bit BD, ±32KB */
        assert(offset >= -32768LL && offset < 32768LL);
        *ptr = (*ptr & 0xFFFC0003) | (((offset >> 2) & 0x3fff) << 2);
    }
    else {
        assert(0); /* unsupported branch type */
    }
}

/* ======================================================================== */
/* Literal pool                                                             */
/* ======================================================================== */
static void add_literal(uintptr_t addr, uintptr_t val)
{
    literals[literalcount][0] = addr;
    literals[literalcount][1] = val;
    literalcount++;
}

static void *add_pointer(void *src, void *addr)
{
    /* On PPC64, pointers embedded inline; no literal pool fixup needed */
    return NULL;
}

static void *kill_pointer(void *stub)
{
    return NULL;
}

static intptr_t get_pointer(void *stub)
{
    return 0;
}

/* ======================================================================== */
/* Register allocation                                                      */
/* ======================================================================== */
static void alloc_reg(struct regstat *cur, int i, signed char tr)
{
    int hr;
    int preferred_reg = (tr & 7);
    if (tr == CCREG) preferred_reg = HOST_CCREG;
    if (tr == PTEMP) preferred_reg = 12;
    if (tr == FTEMP) preferred_reg = 12;
    
    /* Don't allocate unused registers */
    if ((cur->u >> tr) & 1) return;
    
    /* See if it's already allocated */
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (cur->regmap[hr] == tr) return;
    }
    
    /* Keep same mapping if the register was already allocated in a loop */
    preferred_reg = loop_reg(i, tr, preferred_reg);
    
    /* Try to allocate the preferred register */
    if (cur->regmap[preferred_reg] == -1) {
        cur->regmap[preferred_reg] = tr;
        cur->dirty &= ~(1 << preferred_reg);
        cur->isconst &= ~(1 << preferred_reg);
        return;
    }
    
    /* Find a free register */
    for (hr = HOST_REGS - 1; hr >= 0; hr--) {
        if (cur->regmap[hr] == -1) {
            cur->regmap[hr] = tr;
            cur->dirty &= ~(1 << hr);
            cur->isconst &= ~(1 << hr);
            return;
        }
    }
    
    /* Spill a register — prefer unneeded regs, then dirty regs, then LRU */
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (!((cur->u >> cur->regmap[hr]) & 1)) break;
    }
    if (hr >= HOST_REGS) {
        for (hr = 0; hr < HOST_REGS; hr++) {
            if (!(cur->dirty & (1 << hr))) break;
        }
    }
    if (hr >= HOST_REGS) hr = 0;
    /* Spill selected register: store if dirty, then remap */
    if (cur->dirty & (1 << hr)) {
        emit_storereg(cur->regmap[hr], hr);
    }
    cur->regmap[hr] = tr;
    cur->dirty &= ~(1 << hr);
    cur->isconst &= ~(1 << hr);
}

static void alloc_reg64(struct regstat *cur, int i, signed char tr)
{
    /* On PPC64, 64-bit values fit in one GPR, same as 32-bit.
     * But the generic engine expects a separate regmap entry for the
     * upper 32-bit half (tr|64). Allocate both. */
    alloc_reg(cur, i, tr);
    
    /* Don't allocate unused registers */
    if ((cur->uu >> tr) & 1) return;
    
    /* See if the upper half is already allocated */
    int hr;
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (cur->regmap[hr] == (tr|64)) return;
    }
    
    /* Find a free register for the upper half */
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (cur->regmap[hr] == -1) {
            cur->regmap[hr] = tr | 64;
            cur->dirty &= ~(1 << hr);
            cur->isconst &= ~(1 << hr);
            return;
        }
    }
    
    /* Spill a register */
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (!((cur->u >> cur->regmap[hr]) & 1)) break;
    }
    if (hr >= HOST_REGS) {
        for (hr = 0; hr < HOST_REGS; hr++) {
            if (!(cur->dirty & (1 << hr))) break;
        }
    }
    if (hr >= HOST_REGS) hr = 0;
    if (cur->dirty & (1 << hr)) {
        emit_storereg(cur->regmap[hr], hr);
    }
    cur->regmap[hr] = tr | 64;
    cur->dirty &= ~(1 << hr);
    cur->isconst &= ~(1 << hr);
}

static signed char get_reg_arch(struct regstat *cur, int regnum)
{
    int hr;
    for (hr = 0; hr < HOST_REGS; hr++) {
        if (cur->regmap[hr] == regnum) return hr;
    }
    return -1;
}

static void alloc_reg_temp(struct regstat *cur, int i, signed char tr)
{
    alloc_reg(cur, i, (tr < 0) ? PTEMP : tr);
}

static void alloc_cc(struct regstat *cur, int i)
{
    alloc_reg(cur, i, CCREG);
}

/* ======================================================================== */
/* Host register mapping (internal index → PPC register number)            */
/* ======================================================================== */
static const u_int host_reg_ppc[27] = {
     3,  4,  5,  6,  7,  8,  9, 10,
    11, 12, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30
};

/* Translate host register index to PPC register number.
 * Values >= 27 are passed through (they are PPC numbers like FP=31). */
static inline int HREG(int hr) {
    if(hr >= 0 && hr < 27) return host_reg_ppc[hr];
    return hr;
}

/* Inverse: PPC register number → host index (for PPC numbers < 27 that get
 * passed to _HR macros as destination or source registers).
 * Returns the host index that maps to the given PPC number. */
static inline int PPC_HREG(int ppc) {
    int i;
    for(i = 0; i < 27; i++)
        if((int)host_reg_ppc[i] == ppc) return i;
    return ppc;
}

/* ======================================================================== */
/* EMIT macros — PPC64 instruction encoding                                 */
/* ======================================================================== */

/* D-form: opcode(6) | rt(5) | ra(5) | imm(16) */
#define D_FORM(op, rt, ra, imm) \
    (((op) << 26) | ((rt) << 21) | ((ra) << 16) | ((imm) & 0xffff))

/* D-form with host-index to PPC translation */
#define D_FORM_HR(op, rt, ra, imm) \
    (((op) << 26) | (HREG(rt) << 21) | (HREG(ra) << 16) | ((imm) & 0xffff))

/* X-form: opcode(6) | rt(5) | ra(5) | rb(5) | xo(10) | rc(1) */
#define X_FORM(op, rt, ra, rb, xo, rc) \
    (((op) << 26) | ((rt) << 21) | ((ra) << 16) | ((rb) << 11) | ((xo) << 1) | (rc))

/* X-form with host-index translation */
#define X_FORM_HR(op, rt, ra, rb, xo, rc) \
    (((op) << 26) | (HREG(rt) << 21) | (HREG(ra) << 16) | (HREG(rb) << 11) | ((xo) << 1) | (rc))

/* I-form: opcode(6) | li(24) | aa(1) | lk(1) */
#define I_FORM(op, li, aa, lk) \
    (((op) << 26) | (((li) & 0xffffff) << 2) | ((aa) << 1) | (lk))

/* B-form: opcode(6) | bo(5) | bi(5) | bd(14) | aa(1) | lk(1) */
#define B_FORM(op, bo, bi, bd, aa, lk) \
    (((op) << 26) | ((bo) << 21) | ((bi) << 16) | (((bd) & 0x3fff) << 2) | ((aa) << 1) | (lk))

/* MD-form: opcode(6) | rt(5) | ra(5) | rb(5) | mb(5) | me(5) | xo(5) | rc(1) */
#define MD_FORM(op, rt, ra, rb, mb, me, xo, rc) \
    (((op) << 26) | ((rt) << 21) | ((ra) << 16) | ((rb) << 11) | ((mb) << 6) | ((me) << 1) | (rc))

/* MD-form with host-index translation */
#define MD_FORM_HR(op, rt, ra, rb, mb, me, xo, rc) \
    (((op) << 26) | (HREG(rt) << 21) | (HREG(ra) << 16) | (HREG(rb) << 11) | ((mb) << 6) | ((me) << 1) | (rc))

/* MDS-form: opcode(6) | rt(5) | ra(5) | rb(5) | mb(5) | xo(5) | sh(5) | rc(1) */
#define MDS_FORM(op, rt, ra, rb, mb, xo, sh, rc) \
    (((op) << 26) | ((rt) << 21) | ((ra) << 16) | ((rb) << 11) | ((mb) << 6) | ((xo) << 1) | (rc))

/* A-form: opcode(6) | rt(5) | ra(5) | rb(5) | xo(5) | xo2(5) | rc(1) */
/* Same as X-form but with extended opcode */
#define XO_FORM(op, rt, ra, rb, xo, rc) \
    (((op) << 26) | ((rt) << 21) | ((ra) << 16) | ((rb) << 11) | ((xo) << 1) | (rc))

/* XO-form with host-index translation */
#define XO_FORM_HR(op, rt, ra, rb, xo, rc) \
    (((op) << 26) | (HREG(rt) << 21) | (HREG(ra) << 16) | (HREG(rb) << 11) | ((xo) << 1) | (rc))

/* ======================================================================== */
/* Basic PPC opcodes                                                        */
/* ======================================================================== */
/* D-form load/store */
#define OP_LWZ   32   /* lwz    rt, d(ra) */
#define OP_LBZ   34   /* lbz    rt, d(ra) */
#define OP_STW   36   /* stw    rs, d(ra) */
#define OP_STB   38   /* stb    rs, d(ra) */
#define OP_LHZ   40   /* lhz    rt, d(ra) */
#define OP_STH   44   /* sth    rs, d(ra) */
#define OP_LHA   42   /* lha    rt, d(ra) */
#define OP_LWZU  33   /* lwzu   rt, d(ra) */
#define OP_STWU  37   /* stwu   rs, d(ra) */
#define OP_LFS   48   /* lfs    frt, d(ra) */
#define OP_LFD   50   /* lfd    frt, d(ra) */
#define OP_STFS  52   /* stfs   frs, d(ra) */
#define OP_STFD  54   /* stfd   frs, d(ra) */
#define OP_LD    58   /* ld     rt, d(ra) */
#define OP_STD   62   /* std    rs, d(ra) */

/* D-form immediates */
#define OP_ADDI  14   /* addi   rt, ra, simm */
#define OP_ADDIS 15   /* addis  rt, ra, simm */
#define OP_ORI   24   /* ori    ra, rs, uimm */
#define OP_ORIS  25   /* oris   ra, rs, uimm */
#define OP_XORI  26   /* xori   ra, rs, uimm */
#define OP_XORIS 27   /* xoris  ra, rs, uimm */
#define OP_ANDI  28   /* andi.  ra, rs, uimm */
#define OP_ANDIS 29   /* andis. ra, rs, uimm */
#define OP_CMPLI 10   /* cmpli  crf, l, ra, uimm */
#define OP_CMPI  11   /* cmpi   crf, l, ra, simm */
#define OP_SUBFIC 8   /* subfic rt, ra, simm */
#define OP_MULLI 7    /* mulli  rt, ra, simm */

/* I-form branches */
#define OP_B     18   /* b/bl   target */

/* B-form */
#define OP_BC    16   /* bc/bcl target */

/* X-form ALU (XO = extended opcode) */
#define OP_MFXSR 31   /* common major opcode for many X-form insns */

/* X-form sub-opcodes (XO field) */
#define XO_TRAP   4   /* tw/td */
#define XO_MFCR  19   /* mfcr */
#define XO_MTCR  38   /* mtcrf [not fully] */
#define XO_MFMSR 83   /* mfmsr */
#define XO_MTMSR 146  /* mtmsr */
#define XO_MFSR  595  /* mfsr */
#define XO_MTSR  596  /* mtsr */
#define XO_LWZX   23  /* lwzx */
#define XO_LWZUX  55  /* lwzux */
#define XO_LBZX   87  /* lbzx */
#define XO_LBZUX 119  /* lbzux */
#define XO_LHZX  279  /* lhzx */
#define XO_LHZUX 311  /* lhzux */
#define XO_LHAX  343  /* lhax */
#define XO_LHAUX 375  /* lhaux */
#define XO_STWX  151  /* stwx */
#define XO_STWUX 183  /* stwux */
#define XO_STBX  215  /* stbx */
#define XO_STBUX 247  /* stbux */
#define XO_STHX  407  /* sthx */
#define XO_STHUX 439  /* sthux */
#define XO_LWBRX 534  /* lwbrx */
#define XO_STWBRX 662 /* stwbrx */
#define XO_LHBRX 790  /* lhbrx */
#define XO_STHBRX 918 /* sthbrx */
#define XO_LSWX  597  /* lswx */
#define XO_LSWI  597  /* lswi */
#define XO_STSWX 725  /* stswx */
#define XO_STSWI 725  /* stswi */
#define XO_LWARX  20  /* lwarx */
#define XO_STWCX 150  /* stwcx. */
#define XO_MFSPR 339  /* mfspr */
#define XO_MTSPR 467  /* mtspr */
#define XO_SYNC  598  /* sync */
#define XO_ICBI  982  /* icbi */
#define XO_DCBF  86   /* dcbf */
#define XO_DCBST 54   /* dcbst */
#define XO_EIEIO 854  /* eieio */
#define XO_ISYNC 150  /* isync (actually mtspr 0x8A=rd=0x1A4, etc.)*/
#define XO_EXTAB  7   /* extsb */
#define XO_EXTHA  60  /* extsh */
#define XO_EXTWA 443  /* extsw (7c 00 07 74) */

/* XO-form ALU (Rc=0/1) */
#define XO_ADD   266  /* add */
#define XO_ADDC  10   /* addc */
#define XO_ADDE  138  /* adde */
#define XO_ADDME 234  /* addme */
#define XO_ADDZE 202  /* addze */
#define XO_SUBF  40   /* subf */
#define XO_SUBFC 8    /* subfc */
#define XO_SUBFE 136  /* subfe */
#define XO_SUBFME 232 /* subfme */
#define XO_SUBFZE 200 /* subfze */
#define XO_NEG   104  /* neg */
#define XO_MULHW 75   /* mulhw */
#define XO_MULHWU 11  /* mulhwu */
#define XO_MULLW 235  /* mullw */
#define XO_MULHD 73   /* mulhd */
#define XO_MULHDU 9   /* mulhdu */
#define XO_MULLD 233  /* mulld */
#define XO_DIVW  491  /* divw */
#define XO_DIVWU 459  /* divwu */
#define XO_DIVD  489  /* divd */
#define XO_DIVDU 457  /* divdu */
#define XO_AND   28   /* and */
#define XO_ANDC  60   /* andc */
#define XO_NAND  476  /* nand */
#define XO_OR    444  /* or */
#define XO_ORC   412  /* orc */
#define XO_NOR   124  /* nor */
#define XO_XOR   316  /* xor */
#define XO_EQV   284  /* eqv */
#define XO_SLW   24   /* slw */
#define XO_SRW   536  /* srw */
#define XO_SRAW  792  /* sraw */
#define XO_SRAWI 824  /* srawi */
#define XO_SLD   27   /* sld */
#define XO_SRD   539  /* srd */
#define XO_SRAD  794  /* srad */
#define XO_SRADI 413  /* sradi */
#define XO_CNTLZW 26  /* cntlzw */
#define XO_CNTLZD 58  /* cntlzd */

/* ======================================================================== */
/* FPU opcodes (opcode = 59 for single, 63 for double, specials)            */
/* ======================================================================== */
/* FPU extended opcodes (XO field, 10-bit) for single (op=59) & double (op=63) */
#define XO_FADD      21   /* fadd[s]  frD,frA,frB */
#define XO_FSUB      20   /* fsub[s]  frD,frA,frB */
#define XO_FMUL      25   /* fmul[s]  frD,frA,frC */
#define XO_FDIV      18   /* fdiv[s]  frD,frA,frB */
#define XO_FSQRT     22   /* fsqrt[s] frD,0,frB */
#define XO_FABS     264   /* fabs     frD,0,frB */
#define XO_FMR       72   /* fmr      frD,0,frB */
#define XO_FNEG      40   /* fneg     frD,0,frB */
#define XO_FCTIWZ    15   /* fctiwz   frD,0,frB — truncate to int32 */
#define XO_FCTIW     14   /* fctiw    frD,0,frB — current RM to int32 */
#define XO_FRSP      12   /* frsp     frD,0,frB — round to single */
#define XO_FCFID    846   /* fcfid    frD,0,frB — int64 to double */
#define XO_MFFS     583   /* mffs     frD — move from FPSCR */
#define XO_MTFSFI   134   /* mtfsfi   BF,imm — move to FPSCR field imm */
#define XO_FCMPU      0   /* fcmpu    crfD,frA,frB */
#define XO_STFIWX   983   /* stfiwx   frS,ra,rb (opcode=31) */

/* FPR scratch registers (in PPC FPU, registers f0-f31, Linux ABI volatile) */
#define FPR_SCR0  0
#define FPR_SCR1  1
#define FPR_SCR2  2
#define FPR_SCR3  3

/* ======================================================================== */
/* Helper: output 32-bit instruction                                        */
/* ======================================================================== */
static void output_w32(u_int word)
{
    *(u_int *)out = word;
    out += 4;
}

static void output_w64(uint64_t val)
{
    *(uint64_t *)out = val;
    out += 8;
}

/* rlwinm ra, rs, sh, mb, me (opcode 21) — ra, rs are host indices */
#define RLWINM(ra, rs, sh, mb, me, rc) \
    output_w32(0x54000000 | (HREG(rs) << 21) | (HREG(ra) << 16) | ((sh) << 11) | ((mb) << 6) | ((me) << 1) | (rc))

/* slwi ra, rs, n  = rlwinm ra, rs, n, 0, 31-n */
#define EMIT_SLWI(ra, rs, n)   RLWINM(ra, rs, n, 0, 31-(n), 0)

/* srwi ra, rs, n  = rlwinm ra, rs, 32-n, n, 31 */
#define EMIT_SRWI(ra, rs, n)   RLWINM(ra, rs, (32-(n)) & 31, n, 31, 0)

/* ======================================================================== */
/* Helper: load 32-bit sign-extended immediate into register                 */
/* ======================================================================== */
static void emit_movimm(int imm, int rt)
{
    /* For signed values in [-32768, 32767], addi works directly.
     * No zero-extension needed since the result is already correct. */
    if (imm >= -32768 && imm <= 32767) {
        if (imm != 0 || rt != 0) {
            output_w32(D_FORM(OP_ADDI, HREG(rt), 0, imm));
        }
        return;
    }
    /* For unsigned 16-bit values (0..65535), use ori (zero-extend). */
    if ((unsigned)imm <= 0xFFFF) {
        output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
        output_w32(D_FORM_HR(OP_ORI, rt, rt, (unsigned)imm));
        return;
    }
    /* General 32-bit: build with oris + ori (zero-extension avoids
     * addis sign-extension bug when high >= 0x8000). */
    int high = ((unsigned)imm >> 16) & 0xffff;
    int low  = (unsigned)imm & 0xffff;
    output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
    if (high) output_w32(D_FORM_HR(OP_ORIS, rt, rt, high));
    if (low)  output_w32(D_FORM_HR(OP_ORI, rt, rt, low));
}

/* ======================================================================== */
/* 64-bit immediate load (for addresses)                                     */
/* ======================================================================== */
static void emit_zeroreg(int rt)
{
    assem_debug("li r%d, 0", rt);
    /* addi rt, 0, 0 */
    output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
}

static void emit_zeroreg64(int rt)
{
    emit_zeroreg(rt);
}

static void emit_movimm64(uintptr_t imm, int rt)
{
    uint16_t a = (imm >> 48) & 0xffff;
    uint16_t b = (imm >> 32) & 0xffff;
    uint16_t c = (imm >> 16) & 0xffff;
    uint16_t d = imm & 0xffff;
    
    if (imm == 0) {
        output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
        return;
    }
    
    /* 32-bit value (upper 32 bits are zero) — use oris + ori (zero-extend) */
    if (a == 0 && b == 0) {
        if (c == 0) {
            output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
            if (d) output_w32(D_FORM_HR(OP_ORI, rt, rt, d));
        } else {
            output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
            output_w32(D_FORM_HR(OP_ORIS, rt, rt, c));
            if (d) output_w32(D_FORM_HR(OP_ORI, rt, rt, d));
        }
        return;
    }
    
    /* 64-bit constant: build upper 32 bits in lower 32 register bits
     * using ori/oris (zero-extension avoids addis sign-extension bug),
     * then sldi to move to upper 32, then add lower 32 bits. */
    output_w32(D_FORM(OP_ADDI, HREG(rt), 0, 0));
    if (a) output_w32(D_FORM_HR(OP_ORIS, rt, rt, a));
    if (b) output_w32(D_FORM_HR(OP_ORI, rt, rt, b));
    /* sldi rt, rt, 32 = rldicr rt, rt, 32, 31
       MD-form: opcd=30 | RS=rt | RA=rt | SH=32 | ME=31 | xo=1 | Rc=0
       Base: 0x780081F2 (SH=32, ME=31) then OR'd with HREG(rt) for RS,RA */
    output_w32(0x780081F2 | (HREG(rt) << 21) | (HREG(rt) << 16));
    if (c) output_w32(D_FORM_HR(OP_ORIS, rt, rt, c));
    if (d) output_w32(D_FORM_HR(OP_ORI, rt, rt, d));
}

/* ======================================================================== */
/* Register name table (for debug)                                           */
/* ======================================================================== */
#if ASSEM_DEBUG
static char regname[32][4] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10","r11","r12","r13","r14","r15",
    "r16","r17","r18","r19","r20","r21","r22","r23",
    "r24","r25","r26","r27","r28","r29","r30","r31"
};
#endif

/* ======================================================================== */
/* ALU emitters                                                              */
/* ======================================================================== */

static void emit_mov(int rs, int rt)
{
    assem_debug("mr r%d, r%d", rt, rs);
    /* mr rt, rs = or rt, rs, rs
     * XO_FORM_HR: (op, RS, RA, RB, xo, rc)
     *   RS → PPC bits 6-10 (source 1)
     *   RA → PPC bits 11-15 (destination for or)
     *   RB → PPC bits 16-20 (source 2)
     * or RA={rt}, RS={rs}, RB={rs} → or rt, rs, rs → mr rt, rs → rt ← rs */
    output_w32(XO_FORM_HR(OP_MFXSR, rs, rt, rs, XO_OR, 0));
}

static void emit_add(int rs1, int rs2, int rt)
{
    assem_debug("add r%d, r%d, r%d", rt, rs1, rs2);
    output_w32(XO_FORM_HR(OP_MFXSR, rt, rs1, rs2, XO_ADD, 0));
}

static void emit_addc(int rs1, int rs2, int rt)
{
    assem_debug("addc r%d, r%d, r%d", rt, rs1, rs2);
    output_w32(XO_FORM_HR(OP_MFXSR, rt, rs1, rs2, XO_ADDC, 0));
}

static void emit_sub(int rs1, int rs2, int rt)
{
    assem_debug("subf r%d, r%d, r%d", rt, rs2, rs1);
    /* subf rt, rs2, rs1: rt = rs1 - rs2 */
    output_w32(XO_FORM_HR(OP_MFXSR, rt, rs2, rs1, XO_SUBF, 0));
}

static void emit_and(int rs1, int rs2, int rt)
{
    assem_debug("and r%d, r%d, r%d", rt, rs1, rs2);
    /* and RA=rt, RS=rs1, RB=rs2 → rt = rs1 & rs2
     * XO_FORM_HR: (op, RS, RA, RB, xo, rc) */
    output_w32(XO_FORM_HR(OP_MFXSR, rs1, rt, rs2, XO_AND, 0));
}

static void emit_or(int rs1, int rs2, int rt)
{
    assem_debug("or r%d, r%d, r%d", rt, rs1, rs2);
    /* or RA=rt, RS=rs1, RB=rs2 → rt = rs1 | rs2 */
    output_w32(XO_FORM_HR(OP_MFXSR, rs1, rt, rs2, XO_OR, 0));
}

static void emit_xor(int rs1, int rs2, int rt)
{
    assem_debug("xor r%d, r%d, r%d", rt, rs1, rs2);
    /* xor RA=rt, RS=rs1, RB=rs2 → rt = rs1 ^ rs2 */
    output_w32(XO_FORM_HR(OP_MFXSR, rs1, rt, rs2, XO_XOR, 0));
}

static void emit_neg(int rs, int rt)
{
    assem_debug("neg r%d, r%d", rt, rs);
    /* neg rt, rs = subf rt, rs, 0 (XB - XA: 0 - rs = -rs) */
    output_w32(XO_FORM(OP_MFXSR, HREG(rt), HREG(rs), 0, XO_NEG, 0));
}

static void emit_not(int rs, int rt)
{
    assem_debug("nor r%d, r%d, r%d", rt, rs, rs);
    /* nor rt, rs, rs => rt = ~(rs | rs) = ~rs
     * nor RA=rt, RS=rs, RB=rs */
    output_w32(XO_FORM_HR(OP_MFXSR, rs, rt, rs, XO_NOR, 0));
}

static void emit_slt(int rs1, int rs2, int rt)
{
    /* cmpw cr0, rs1, rs2; mfcr rt; rlwinm rt, rt, 29, 31, 31 */
    /* CR0[LT] = bit 31 of CR, extracted by rotating right 29 then masking */
    assem_debug("slt r%d, r%d, r%d", rt, rs1, rs2);
    emit_cmp(rs1, rs2);              /* cmpw cr0, rs1, rs2 */
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));  /* mfcr rt */
    /* rlwinm rt, rt, 29, 31, 31: extract bit 61 (CR0[LT]) to bit 31 */
    RLWINM(rt, rt, 29, 31, 31, 0);
}

static void emit_sltu(int rs1, int rs2, int rt)
{
    /* cmplw cr0, rs1, rs2; mfcr rt; rlwinm rt, rt, 29, 31, 31 */
    assem_debug("sltu r%d, r%d, r%d", rt, rs1, rs2);
    emit_cmpu(rs1, rs2);
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));
    RLWINM(rt, rt, 29, 31, 31, 0);
}

static void emit_slti(int rs, int imm, int rt)
{
    assem_debug("slti r%d, r%d, %d", rt, rs, imm);
    emit_cmpimm(rs, imm);
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));
    RLWINM(rt, rt, 29, 31, 31, 0);
}

static void emit_sltiu(int rs, int imm, int rt)
{
    assem_debug("sltiu r%d, r%d, %d", rt, rs, imm);
    emit_cmpuimm(rs, imm);
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));
    RLWINM(rt, rt, 29, 31, 31, 0);
}

/* ======================================================================== */
/* Branch emitters                                                          */
/* ======================================================================== */

static void emit_b(intptr_t addr)
{
    assem_debug("b %x", addr);
    if(addr < 4) {
        output_w32(I_FORM(OP_B, 0, 0, 0));
    } else {
        intptr_t offset = addr - (intptr_t)out;
        if (offset >= -33554432LL && offset < 33554432LL) {
            output_w32(I_FORM(OP_B, (offset >> 2), 0, 0));
        } else {
            /* Out of range — use absolute indirect jump */
            emit_movimm64(addr, HOST_TEMPREG);
            output_w32(X_FORM(31, HREG(HOST_TEMPREG), 9, 0, XO_MTSPR, 0)); /* mtctr r12 */
            output_w32(0x4E800420); /* bctr */
        }
    }
}

static void emit_bl(intptr_t addr)
{
    assem_debug("bl %x", addr);
    if(addr < 4) {
        output_w32(I_FORM(OP_B, 0, 0, 1));
    } else {
        intptr_t offset = addr - (intptr_t)out;
        if (offset >= -33554432LL && offset < 33554432LL) {
            output_w32(I_FORM(OP_B, (offset >> 2), 0, 1));
        } else {
            /* Out of range — use absolute indirect call (bctrl preserves LR) */
            emit_movimm64(addr, HOST_TEMPREG);
            output_w32(X_FORM(31, HREG(HOST_TEMPREG), 9, 0, XO_MTSPR, 0)); /* mtctr r12 */
            output_w32(0x4E800421); /* bctrl (bcctrl 20,0 with LK=1) */
        }
    }
}

static void emit_bne(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bne %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* bne cr0: BO=4 (branch if false), BI=cr0*4+2 (EQ bit) */
    output_w32(B_FORM(OP_BC, 4, 2, (offset >> 2), 0, 0));
}

static void emit_beq(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("beq %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* beq cr0: BO=12 (branch if true), BI=cr0*4+2 (EQ bit) */
    output_w32(B_FORM(OP_BC, 12, 2, (offset >> 2), 0, 0));
}

static void emit_bgt(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bgt %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* bgt cr0: BO=12 (branch if true), BI=cr0*4+1 (GT bit) */
    output_w32(B_FORM(OP_BC, 12, 1, (offset >> 2), 0, 0));
}

static void emit_blt(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("blt %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* blt cr0: BO=12 (branch if true), BI=cr0*4+0 (LT bit) */
    output_w32(B_FORM(OP_BC, 12, 0, (offset >> 2), 0, 0));
}

static void emit_bge(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bge %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* bge cr0: BO=4 (branch if false), BI=cr0*4+0 (LT bit) */
    output_w32(B_FORM(OP_BC, 4, 0, (offset >> 2), 0, 0));
}

static void emit_ble(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("ble %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    /* ble cr0: BO=4 (branch if false), BI=cr0*4+1 (GT bit) */
    output_w32(B_FORM(OP_BC, 4, 1, (offset >> 2), 0, 0));
}

static void emit_bnecr(int cr, intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bne cr%d, %x", cr, addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 4, cr*4+2, (offset >> 2), 0, 0));
}

static void emit_beqcr(int cr, intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("beq cr%d, %x", cr, addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, cr*4+2, (offset >> 2), 0, 0));
}

static void emit_bgtcr(int cr, intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bgt cr%d, %x", cr, addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, cr*4+1, (offset >> 2), 0, 0));
}

static void emit_bltcr(int cr, intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("blt cr%d, %x", cr, addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, cr*4+0, (offset >> 2), 0, 0));
}

static void emit_bne_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bne- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 4, 2, (offset >> 2), 0, 0));
}

static void emit_beq_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("beq- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, 2, (offset >> 2), 0, 0));
}

static void emit_bgt_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bgt- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, 1, (offset >> 2), 0, 0));
}

static void emit_blt_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("blt- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 12, 0, (offset >> 2), 0, 0));
}

static void emit_bge_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("bge- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 4, 0, (offset >> 2), 0, 0));
}

static void emit_ble_ds(intptr_t addr)
{
    intptr_t offset = addr - (intptr_t)out;
    assem_debug("ble- %x", addr);
    assert(offset >= -32768LL && offset < 32768LL);
    output_w32(B_FORM(OP_BC, 4, 1, (offset >> 2), 0, 0));
}

static void emit_jno_valid(intptr_t addr)
{
    /* Unconditional jump (used for verified valid entries) */
    emit_jmp(addr);
}

static void emit_jno_null(intptr_t addr)
{
    /* Unconditional jump (used for null check — the caller must have
     * emitted a conditional skip before this so we only get here when
     * the pointer is non-null). Uses emit_jmp to handle far targets. */
    emit_jmp(addr);
}

static void emit_call(intptr_t addr)
{
    assem_debug("emit_call %p", (void*)addr);
    /* Uses full 64-bit address via r12 + mtctr + bctrl.
     * This is necessary because the mmap'd code buffer can be
     * gigabytes away from the text section (beyond bl ±32MB range). */
    emit_64bit_call(addr, HOST_TEMPREG);
}

/* ======================================================================== */
/* Compare / test emitters                                                  */
/* ======================================================================== */

/* cmpw cr0, rs, rt: set CR0 based on rs vs rt (32-bit signed) */
static void emit_cmp(int rs, int rt)
{
    assem_debug("cmpw cr0, r%d, r%d", rs, rt);
    /* cmp (BF=0, L=0): (31<<26) | (0<<23) | (1<<22) | (0<<21) | (rs<<16) | (rt<<11) */
    output_w32(0x7C000000 | (HREG(rs) << 16) | (HREG(rt) << 11));
}

static void emit_cmpimm(int rs, int imm)
{
    if(imm == 0x800000) {
        /* KSEG0 check for memory fast path (used before emit_jno).
         * On PPC64 there is no overflow flag, so we test bit 31 directly
         * via andis. instead of a signed cmpwi (which can't distinguish
         * the signed-negative KSEG0 range from small positive values).
         * andis. r0, rs, 0x8000 → r0 = rs & 0x80000000, sets CR0[EQ]. */
        assem_debug("andis. r0, r%d, 0x8000  (KSEG0 check)", rs);
        output_w32(D_FORM(OP_ANDIS, 0, HREG(rs), 0x8000));
    } else {
        assem_debug("cmpwi cr0, r%d, %d", rs, imm);
        output_w32(D_FORM(OP_CMPI, 0, HREG(rs), imm));
    }
}

static void emit_cmpu(int rs, int rt)
{
    assem_debug("cmplw cr0, r%d, r%d", rs, rt);
    /* cmpl (BF=0, L=0): (31<<26) | (0<<23) | (1<<22) | (0<<21) | (rs<<16) | (rt<<11) | (32<<1) */
    /* cmpl has xo=32, cmp has xo=0 */
    /* Wait: cmp = (opcode 31, xo 0), cmpl = (opcode 31, xo 32) */
    output_w32(0x7C000000 | (1<<22) | (HREG(rs) << 16) | (HREG(rt) << 11) | (32 << 1));
}

static void emit_cmpuimm(int rs, int imm)
{
    assem_debug("cmplwi cr0, r%d, %d", rs, imm);
    /* cmpli BF=0, L=0, ra=rs, uimm */
    output_w32(D_FORM(OP_CMPLI, 0, HREG(rs), imm));
}

/* cmpd cr0, rs, rt: 64-bit signed compare */
static void emit_cmp64(int rs, int rt)
{
    assem_debug("cmpd cr0, r%d, r%d", rs, rt);
    /* cmp (BF=0, L=1 for 64-bit): (31<<26) | (0<<23) | (1<<22) | (1<<21) | (rs<<16) | (rt<<11) */
    output_w32(0x7C200000 | (HREG(rs) << 16) | (HREG(rt) << 11));
}

static void emit_test(int rs, int rt)
{
    assem_debug("and. r0, r%d, r%d", rs, rt);
    /* and. r0, rs, rt (dot form sets CR0) */
    output_w32(XO_FORM(OP_MFXSR, HREG(rs), 0, HREG(rt), XO_AND, 1));
}

static void emit_testimm(int rs, int imm)
{
    assem_debug("andi. r0, r%d, %d", rs, imm);
    /* andi. r0, rs, uimm */
    output_w32(D_FORM(OP_ANDI, HREG(rs), 0, imm));
}

/* ======================================================================== */
/* Add immediate / load address                                              */
/* ======================================================================== */

static void emit_addimm(int rs, int imm, int rt)
{
    if (imm == 0) {
        if (rs != rt) emit_mov(rs, rt);
        return;
    }
    if (imm > 0 && imm < 65536) {
        if (imm <= 32767) {
            output_w32(D_FORM_HR(OP_ADDI, rt, rs, imm));
        } else {
            /* addis rs+65536, then addi neg to get rs+imm */
            output_w32(D_FORM_HR(OP_ADDIS, rt, rs, 1));
            output_w32(D_FORM_HR(OP_ADDI, rt, rt, imm - 65536));
        }
        return;
    }
    if (imm < 0 && imm > -65536) {
        if (imm >= -32768) {
            output_w32(D_FORM_HR(OP_ADDI, rt, rs, imm));
        } else {
            int high = (imm + 0x8000) >> 16; /* rounding */
            int low = imm - (high << 16);
            output_w32(D_FORM_HR(OP_ADDIS, rt, rs, high & 0xffff));
            if (low)
                output_w32(D_FORM_HR(OP_ADDI, rt, rt, low));
        }
        return;
    }
    /* Load immediate into temp register first */
    emit_movimm(imm, HOST_TEMPREG);
    emit_add(rs, HOST_TEMPREG, rt);
}

/* ======================================================================== */
/* 64-bit add immediate (alias: addi works in 64-bit mode on PPC64)          */
/* ======================================================================== */
static void emit_addimm64(int rs, int imm, int rt)
{
    assem_debug("addi64 r%d, r%d, %d", rt, rs, imm);
    emit_addimm(rs, imm, rt);
}

/* ======================================================================== */
/* Add immediate and set flags (adds + cmpwi to set CR0)                     */
/* ======================================================================== */
static void emit_addimm_and_set_flags(int imm, int rt)
{
    assem_debug("addi_and_set_flags r%d, %d", rt, imm);
    emit_addimm(rt, imm, rt);
    emit_cmpimm(rt, 0);
}

/* ======================================================================== */
/* Jump table helpers — redirect far targets through jump table at end of   */
/* code buffer (matching arch_init layout: 32 bytes per entry).              */
/* ======================================================================== */

/* Find a symbol in jump_table_symbols, return its index or -1. */
static int find_jump_table_entry(intptr_t addr)
{
    int n;
    int nentries = sizeof(jump_table_symbols) / sizeof(jump_table_symbols[0]);
    for (n = 0; n < nentries; n++)
        if (addr == jump_table_symbols[n]) return n;
    return -1;
}

/* Compute the PC-relative branch offset (in word-count form for I_FORM/B_FORM)
 * to reach `addr` from the current `out` position.
 * For targets outside ±32MB (B instruction) or ±32KB (BC instruction),
 * redirects through the jump table.  Returns the offset in "words" (>>2). */
static intptr_t genjmp(intptr_t addr)
{
    if (addr < 4) return 0;
    intptr_t out_rx = (intptr_t)out;
    /* If target is outside the code buffer, adjust for RX mirror */
    if (addr < (intptr_t)base_addr || addr >= (intptr_t)base_addr + (1 << TARGET_SIZE_2))
        out_rx = ((intptr_t)out - (intptr_t)base_addr) + (intptr_t)base_addr_rx;
    intptr_t offset = addr - out_rx;
    if (offset < -33554432LL || offset >= 33554432LL) {
        int n = find_jump_table_entry(addr);
        if (n >= 0) {
            intptr_t entry_addr = (intptr_t)base_addr_rx + (1 << TARGET_SIZE_2) - JUMP_TABLE_SIZE + n * JUMP_TABLE_ENTRY_SIZE;
            offset = entry_addr - out_rx;
        }
    }
    return offset >> 2;
}

/* ======================================================================== */
/* Shift emitters                                                            */
/* ======================================================================== */

static void emit_shlimm(int rs, unsigned int imm, int rt)
{
    assem_debug("slwi r%d, r%d, %d", rt, rs, imm);
    if (imm >= 32) { rt = 0; return; } /* shift > 31 = 0 for 32-bit */
    EMIT_SLWI(rt, rs, imm);
}

static void emit_rlwinm(int rt, int rs, int sh, int mb, int me)
{
    RLWINM(rt, rs, sh, mb, me, 0);
}

static void emit_shlimm64(int rs, unsigned int imm, int rt)
{
    assem_debug("sldi r%d, r%d, %d", rt, rs, imm);
    /* rldicr rt, rs, sh, 63-sh (shift left and clear right) */
    /* MD-form: opcd=30(0-5), RS(6-10), RA(11-15), sh(16-20), me(21-25), xo=1(26-30), Rc=0(31) */
    int sh = imm & 63;
    int me = (63 - sh) & 63;
    output_w32(0x78000002 | (HREG(rs) << 21) | (HREG(rt) << 16)
        | ((sh & 0x1f) << 11) | (((sh >> 5) & 1) << 10)
        | ((me & 0x1f) << 6));
}

/* Right shift (logical): srwi ra, rs, n */
static void emit_shrimm(int rs, unsigned int imm, int rt)
{
    assem_debug("srwi r%d, r%d, %d", rt, rs, imm);
    if (imm >= 32) { emit_zeroreg(rt); return; }
    EMIT_SRWI(rt, rs, imm);
}

/* Right shift (arithmetic): srawi rt, rs, imm */
static void emit_sarimm(int rs, unsigned int imm, int rt)
{
    assem_debug("srawi r%d, r%d, %d", rt, rs, imm);
    if (imm >= 32) imm = 31; /* max shift for srawi */
    /* srawi rt, rs, imm: opcode 31, xo=824
     * srawi RA=rt, RS=rs, SH=imm
     * X_FORM: (op, RS_field, RA_field, RB_field, xo, rc) */
    output_w32(X_FORM(31, HREG(rs), HREG(rt), imm, XO_SRAWI, 0));
}

/* Rotate right: rlwinm rt, rs, 32-n, 0, 31 */
static void emit_rorimm(int rs, unsigned int imm, int rt)
{
    assem_debug("rotlwi r%d, r%d, %d", rt, rs, (32 - imm) & 31);
    /* On PPC, rotlwi = rlwinm rt, rs, n, 0, 31 where n is left rotate amount */
    /* For right rotate by imm, use left rotate by (32-imm) */
    RLWINM(rt, rs, (32 - imm) & 31, 0, 31, 0);
}

/* 64-bit variants */
static void emit_shrimm64(int rs, unsigned int imm, int rt)
{
    assem_debug("srdi r%d, r%d, %d", rt, rs, imm);
    if (imm >= 64) { emit_zeroreg(rt); return; }
    /* srdi: rldicl rt, rs, 64-imm, imm */
    /* MD-form: opcd=30(0-5), RS(6-10), RA(11-15), sh(16-20), mb(21-25), xo=0(26-30), Rc=0(31) */
    int sh = (64 - imm) & 63;
    int mb = imm & 63;
    output_w32(0x78000000 | (HREG(rs) << 21) | (HREG(rt) << 16)
        | ((sh & 0x1f) << 11) | (((sh >> 5) & 1) << 10)
        | ((mb & 0x1f) << 6));
}

static void emit_sarimm64(int rs, unsigned int imm, int rt)
{
    assem_debug("sradi r%d, r%d, %d", rt, rs, imm);
    if (imm >= 64) imm = 63;
    /* sradi rt, rs, imm: sradi RA=rt, RS=rs, SH=imm
     * X_FORM: (op, RS_field, RA_field, RB_field, xo, rc) */
    int sh_hi = (imm >> 5) & 1;
    int sh_lo = imm & 31;
    output_w32(X_FORM(31, HREG(rs), HREG(rt), sh_lo | (sh_hi << 5), XO_SRADI + sh_hi, 0));
}

static void emit_rorimm64(int rs, unsigned int imm, int rt)
{
    assem_debug("rotrdi r%d, r%d, %d", rt, rs, imm);
    /* rldicl rt, rs, 64-imm, 0 */
    int sh = (64 - imm) & 63;
    output_w32(0x78000000 | (HREG(rs) << 21) | (HREG(rt) << 16)
        | ((sh & 0x1f) << 11) | (((sh >> 5) & 1) << 10));
}

/* Set if less than (signed) */
static void emit_set_if_less32(int rs1, int rs2, int rt)
{
    assem_debug("set_if_less r%d, r%d -> r%d", rs1, rs2, rt);
    emit_cmp(rs1, rs2);
    /* mfcr rt; rlwinm rt, rt, 29, 31, 31 */
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));
    RLWINM(rt, rt, 29, 31, 31, 0);
}

/* Set if carry (unsigned less than) */
static void emit_set_if_carry32(int rs1, int rs2, int rt)
{
    assem_debug("set_if_carry r%d, r%d -> r%d", rs1, rs2, rt);
    emit_cmpu(rs1, rs2);
    output_w32(X_FORM(OP_MFXSR, HREG(rt), 0, 0, XO_MFCR, 0));
    RLWINM(rt, rt, 29, 31, 31, 0);
}

/* ======================================================================== */
/* Conditional move / select                                                 */
/* ======================================================================== */

/* On PPC, there's no direct conditional move like ARM64's csel.
 * Instead we use: bc !cond, +8; ori rt, src, 0 (skip when condition false)
 * or: subf rt, rt, rt (zero rt); bc cond, +8; ori rt, src, 0 */

static void emit_cmovne_imm(int imm, int rt)
{
    /* if Z flag clear (NE), set rt = imm; else rt unchanged */
    assem_debug("bne+ 8; li r%d, %d", rt, imm);
    emit_bne_ds((intptr_t)(out + 8)); /* skip next if not equal */
    emit_movimm(imm, rt);
    /* Note: out+8 means skip 2 instructions = 8 bytes, which requires
     * the offset to be within ±32KB. Since we're computing forward, this works. */
}

static void emit_cmovl_imm(int imm, int rt)
{
    assem_debug("blt+ 8; li r%d, %d", rt, imm);
    intptr_t skip = (intptr_t)(out + 8);
    emit_blt_ds(skip);
    emit_movimm(imm, rt);
}

static void emit_cmovb_imm(int imm, int rt)
{
    assem_debug("blt+ 8; li r%d, %d", rt, imm);
    intptr_t skip = (intptr_t)(out + 8);
    emit_blt_ds(skip);
    emit_movimm(imm, rt);
}

static void emit_cmovs_imm(int imm, int rt)
{
    assem_debug("blt+ 8; li r%d, %d", rt, imm);
    intptr_t skip = (intptr_t)(out + 8);
    emit_blt_ds(skip);
    emit_movimm(imm, rt);
}

/* ======================================================================== */
/* Load/store with absolute address                                          */
/* ======================================================================== */

static void emit_readword(intptr_t addr, int rt)
{
    assem_debug("load_word 0x%x -> r%d", (unsigned int)addr, rt);
    /* Load absolute address into temp register, then lwz */
    emit_movimm64(addr, HOST_TEMPREG);
    emit_readword_indexed(0, HOST_TEMPREG, rt);
}

static void emit_writeword(int rt, intptr_t addr)
{
    assem_debug("store r%d -> 0x%x", rt, (unsigned int)addr);
    emit_movimm64(addr, HOST_TEMPREG);
    emit_writeword_indexed(rt, 0, HOST_TEMPREG);
}

static void emit_readword_unaligned(intptr_t addr, int rt)
{
    emit_readword(addr, rt);
}

static void emit_writedword(int rt, intptr_t addr)
{
    if(rt<0) return;
    intptr_t offset = addr - (intptr_t)&g_dev.r4300.new_dynarec_hot_state;
    assert(offset < 32768LL);
    assert(offset >= -32768LL);
    assert((offset & 7) == 0);
    assem_debug("std r%d, %d(r31)", rt, (int)offset);
    output_w32(D_FORM_HR(OP_STD, rt, FP, offset));
}

/* ======================================================================== */
/* Architecture initialization                                               */
/* ======================================================================== */
static void arch_init(void)
{
    fprintf(stderr, "PPC64: arch_init called, base_addr=%p, base_addr_rx=%p, out=%p\n",
            base_addr, base_addr_rx, out);
    assert((fp_memory_map & 7) == 0);
    
    g_dev.r4300.new_dynarec_hot_state.rounding_modes[0] = 0x00000000; /* round (RN=00) */
    g_dev.r4300.new_dynarec_hot_state.rounding_modes[1] = 0x40000000; /* trunc (RN=01) */
    g_dev.r4300.new_dynarec_hot_state.rounding_modes[2] = 0x80000000; /* ceil (RN=10) */
    g_dev.r4300.new_dynarec_hot_state.rounding_modes[3] = 0xC0000000; /* floor (RN=11) */
    
#ifdef RAM_OFFSET
    g_dev.r4300.new_dynarec_hot_state.ram_offset = 
        (intptr_t)g_dev.rdram.dram - (intptr_t)0x80000000;
#endif
    
    /* Populate jump table with function pointers */
    jump_table_symbols[0]  = (intptr_t)cached_interp_TLBR;
    jump_table_symbols[1]  = (intptr_t)cached_interp_TLBP;
    jump_table_symbols[2]  = (intptr_t)cached_interp_MULT;
    jump_table_symbols[3]  = (intptr_t)cached_interp_MULTU;
    jump_table_symbols[4]  = (intptr_t)cached_interp_DIV;
    jump_table_symbols[5]  = (intptr_t)cached_interp_DIVU;
    jump_table_symbols[6]  = (intptr_t)cached_interp_DMULT;
    jump_table_symbols[7]  = (intptr_t)cached_interp_DMULTU;
    jump_table_symbols[8]  = (intptr_t)cached_interp_DDIV;
    jump_table_symbols[9]  = (intptr_t)cached_interp_DDIVU;
    
    /* Write jump table at end of code buffer.
     * Each entry is JUMP_TABLE_ENTRY_SIZE (32) bytes:
     *
     * Direct branch (target within ±32MB of RX mirror):
     *   [0]  b <target>   (PC-relative branch)
     *   [1-7] nop         (padding)
     *
     * Far branch (target outside ±32MB of RX mirror):
     *   [0]  bl .+20      (branch+link forward 4 insns to label 1)
     *   [1]  b .+24       (skip over data + loader, jump past entry)
     *   [2-3] .quad target (8-byte target address)
     *   [4]  1: mflr r12  (r12 = &slot[4])
     *   [5]  ld r12, -8(r12) (r12 = slot[4-2] = slot[2] = target)
     *   [6]  mtctr r12
     *   [7]  bctr
     *
     * The RX mirror advances by JUMP_TABLE_ENTRY_SIZE per entry.
     */
    int nentries = sizeof(jump_table_symbols) / sizeof(jump_table_symbols[0]);
    u_char  *write_ptr = (u_char *)base_addr + (1 << TARGET_SIZE_2) - JUMP_TABLE_SIZE;
    u_int   *rx_ptr    = (u_int   *)((char *)base_addr_rx + (1 << TARGET_SIZE_2) - JUMP_TABLE_SIZE);
    int i;
    for (i = 0; i < nentries; i++) {
        uintptr_t target = (uintptr_t)jump_table_symbols[i];
        u_int *slot = (u_int *)write_ptr;
        intptr_t offset_from_rx = (intptr_t)target - (intptr_t)rx_ptr;
        if (offset_from_rx >= -33554432LL && offset_from_rx < 33554432LL) {
            /* Direct branch: LI = word_offset & 0xFFFFFF, inserted at bits 25-2 */
            slot[0] = 0x48000000 | (((offset_from_rx >> 2) & 0xFFFFFF) << 2); /* b target */
            slot[1] = 0x60000000;
            slot[2] = 0x60000000;
            slot[3] = 0x60000000;
            slot[4] = 0x60000000;
            slot[5] = 0x60000000;
            slot[6] = 0x60000000;
            slot[7] = 0x60000000;
        } else {
            /* Far branch: bl 1f; b .+24; .quad target; 1: mflr r12; ld r12, 4(r12); mtctr r12; bctr
             * bl .+16 → branches to slot[4]; LR = slot[1] (after bl).
             * mflr r12 → r12 = &slot[1].
             * ld r12, 4(r12) → loads 8 bytes from &slot[2] (= target address). */
            slot[0] = 0x48000011; /* bl .+16 (forward to slot[4]) */
            slot[1] = 0x4800001C; /* b .+28 (skip over data at slots[2-3] + loader at slots[4-7], land after entry) */
            ((u_int *)slot)[2] = (u_int)(target >> 32);  /* hi32(target) */
            ((u_int *)slot)[3] = (u_int)(target & 0xFFFFFFFF); /* lo32(target) */
            slot[4] = 0x7D8802A6; /* 1: mflr r12 */
            slot[5] = 0xE98C0004; /* ld r12, 4(r12) */
            slot[6] = 0x7D8903A6; /* mtctr r12 */
            slot[7] = 0x4E800420; /* bctr */
    }
    write_ptr += JUMP_TABLE_ENTRY_SIZE;
        rx_ptr += JUMP_TABLE_ENTRY_SIZE / 4; /* advance by 8 u_int = 32 bytes */
    }
    /* Flush dcache + invalidate icache for the jump table region */
    cache_flush((char *)base_addr + (1 << TARGET_SIZE_2) - JUMP_TABLE_SIZE,
                (char *)base_addr + (1 << TARGET_SIZE_2));
}

/* ======================================================================== */
/* Load/store emitters                                                      */
/* ======================================================================== */

/* Indexed load: lwzx rt, ra, rb */
static void emit_readword_indexed(int offset, int rs, int rt)
{
    assem_debug("lwz r%d, %d(r%d)", rt, offset, rs);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_LWZ, rt, rs, offset));
}

/* Indexed store: stw rs, offset, ra */
static void emit_writeword_indexed(int rs, int offset, int ra)
{
    assem_debug("stw r%d, %d(r%d)", rs, offset, ra);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_STW, rs, ra, offset));
}

/* lwzx rt, ra, rb */
static void emit_readword_dualindexed(int rs1, int rs2, int rt)
{
    assem_debug("lwzx r%d, r%d, r%d", rt, rs1, rs2);
    output_w32(X_FORM_HR(OP_MFXSR, rt, rs1, rs2, XO_LWZX, 0));
}

/* stwx rs, ra, rb */
static void emit_writeword_dualindexed(int rs, int rs1, int rs2)
{
    assem_debug("stwx r%d, r%d, r%d", rs, rs1, rs2);
    output_w32(X_FORM_HR(OP_MFXSR, rs, rs1, rs2, XO_STWX, 0));
}

/* 64-bit load: ld rt, offset(ra) */
static void emit_readdword_indexed(int offset, int rs, int rt)
{
    assem_debug("ld r%d, %d(r%d)", rt, offset, rs);
    assert(offset >= -32768 && offset < 32768);
    /* ld: opcode 58, rt<<21, ra<<16, ds<<2 (DS must be word-aligned >>2) */
    assert((offset & 3) == 0);
    output_w32((58 << 26) | (HREG(rt) << 21) | (HREG(rs) << 16) | ((offset & 0xffff)));
}

/* 64-bit store: std rs, offset(ra) */
static void emit_writedword_indexed(int rs, int offset, int ra)
{
    assem_debug("std r%d, %d(r%d)", rs, offset, ra);
    assert(offset >= -32768 && offset < 32768);
    assert((offset & 3) == 0);
    output_w32((62 << 26) | (HREG(rs) << 21) | (HREG(ra) << 16) | ((offset & 0xffff)));
}

/* ldx rt, ra, rb */
static void emit_readdword_dualindexed(int rs1, int rs2, int rt)
{
    assem_debug("ldx r%d, r%d, r%d", rt, rs1, rs2);
    /* ldx: opcode 31, xo=21 */
    output_w32(X_FORM_HR(OP_MFXSR, rt, rs1, rs2, 21, 0));
}

/* stdx rs, ra, rb */
static void emit_writedword_dualindexed(int rs, int rs1, int rs2)
{
    assem_debug("stdx r%d, r%d, r%d", rs, rs1, rs2);
    /* stdx: opcode 31, xo=149 */
    output_w32(X_FORM_HR(OP_MFXSR, rs, rs1, rs2, 149, 0));
}

/* Load byte: lbz rt, offset(ra) */
static void emit_readbyte_indexed(int offset, int rs, int rt)
{
    assem_debug("lbz r%d, %d(r%d)", rt, offset, rs);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_LBZ, rt, rs, offset));
}

/* Load halfword: lhz rt, offset(ra) */
static void emit_readhword_indexed(int offset, int rs, int rt)
{
    assem_debug("lhz r%d, %d(r%d)", rt, offset, rs);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_LHZ, rt, rs, offset));
}

/* Store byte: stb rs, offset(ra) */
static void emit_writebyte_indexed(int rs, int offset, int ra)
{
    assem_debug("stb r%d, %d(r%d)", rs, offset, ra);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_STB, rs, ra, offset));
}

/* Store halfword: sth rs, offset(ra) */
static void emit_writehword_indexed(int rs, int offset, int ra)
{
    assem_debug("sth r%d, %d(r%d)", rs, offset, ra);
    assert(offset >= -32768 && offset < 32768);
    output_w32(D_FORM_HR(OP_STH, rs, ra, offset));
}

/* TLB-aware indexed load: lwzx or lwz based on whether mapping table is active */
static void emit_readword_indexed_tlb(int addr, int rs, int map, int rt)
{
    assert(map >= 0);
    if (map < 0) {
        emit_readword_indexed(addr, rs, rt);
    } else {
        assert(addr == 0);
        emit_readword_dualindexed(rs, map, rt);
    }
}

/* TLB-aware indexed store: stwx or stw */
static void emit_writeword_indexed_tlb(int rs, int addr, int ra, int map)
{
    assert(map >= 0);
    if (map < 0) {
        emit_writeword_indexed(rs, addr, ra);
    } else {
        assert(addr == 0);
        emit_writeword_dualindexed(rs, ra, map);
    }
}

/* TLB-aware byte load */
static void emit_readbyte_indexed_tlb(int addr, int rs, int map, int rt)
{
    assert(map >= 0);
    if (map < 0) {
        emit_readbyte_indexed(addr, rs, rt);
    } else {
        if (addr == 0) {
            emit_readword_dualindexed(rs, map, HOST_TEMPREG);
            emit_rlwinm(rt, HOST_TEMPREG, 0, 24, 31); /* extract low byte */
        } else {
            emit_addimm(map, addr, HOST_TEMPREG);
            emit_readbyte_indexed(0, HOST_TEMPREG, rt);
        }
    }
}

/* TLB-aware halfword load */
static void emit_readhword_indexed_tlb(int addr, int rs, int map, int rt)
{
    assert(map >= 0);
    if (map < 0) {
        emit_readhword_indexed(addr, rs, rt);
    } else {
        if (addr == 0) {
            emit_readword_dualindexed(rs, map, HOST_TEMPREG);
            emit_rlwinm(rt, HOST_TEMPREG, 0, 16, 31); /* extract low halfword */
        } else {
            emit_addimm(map, addr, HOST_TEMPREG);
            emit_readhword_indexed(0, HOST_TEMPREG, rt);
        }
    }
}

/* TLB-aware byte store */
static void emit_writebyte_indexed_tlb(int rs, int addr, int ra, int map)
{
    assert(map >= 0);
    if (map < 0) {
        emit_writebyte_indexed(rs, addr, ra);
    } else {
        assert(addr == 0);
        emit_writeword_dualindexed(rs, ra, map);
    }
}

/* TLB-aware halfword store */
static void emit_writehword_indexed_tlb(int rs, int addr, int ra, int map)
{
    assert(map >= 0);
    if (map < 0) {
        emit_writehword_indexed(rs, addr, ra);
    } else {
        assert(addr == 0);
        /* Can't directly store halfword via dual-index; use temp register */
        emit_readword_dualindexed(ra, map, HOST_TEMPREG);
        emit_rlwinm(rs, rs, 0, 16, 31); /* mask to 16 bits */
        emit_or(HOST_TEMPREG, rs, HOST_TEMPREG);
        emit_writeword_dualindexed(HOST_TEMPREG, ra, map);
    }
}

/* ======================================================================== */
/* Placeholder branches — patched later by set_jump_target                   */
/* ======================================================================== */
/* BC conditional branch base encodings (AA=0, LK=0) */
#define BC_BASE(bo, bi) (0x40000000 | ((bo)&0x1f)<<21 | ((bi)&0x1f)<<16)
/* BO values for bc instruction:
 * BT_BO=12: bt — branch if condition true (CR bit = 1)
 * BF_BO=4:  bf — branch if condition false (CR bit = 0) */
#define BT_BO 12
#define BF_BO 4

static void emit_jmp(intptr_t a)
{
    assem_debug("b %x", a);
    if(a < 4) {
        output_w32(I_FORM(OP_B, 0, 0, 0));
    } else {
        intptr_t li = genjmp(a);
        /* Check if li fits in the 24-bit signed LI field of the B instruction.
         * 24-bit signed range: -0x800000 to 0x7FFFFF (words, ≈ ±32 MB).
         * If genjmp returned an out-of-range offset (no jump table entry
         * for an external target like do_interrupt), use an indirect jump. */
        if (li > -0x800000 && li < 0x800000) {
            output_w32(I_FORM(OP_B, li, 0, 0));
        } else {
            /* Out of range — use absolute indirect jump via r12 */
            emit_movimm64(a, HOST_TEMPREG);
            output_w32(X_FORM(31, HREG(HOST_TEMPREG), 9, 0, XO_MTSPR, 0)); /* mtctr r12 */
            output_w32(0x4E800420); /* bctr */
        }
    }
}

static void emit_jeq(intptr_t a)
{
    assem_debug("beq %x", a);
    if(a < 4) {
        output_w32(BC_BASE(12, 2));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(12, 2) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jne(intptr_t a)
{
    assem_debug("bne %x", a);
    if(a < 4) {
        output_w32(BC_BASE(4, 2));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(4, 2) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jl(intptr_t a)
{
    assem_debug("blt %x", a);
    if(a < 4) {
        output_w32(BC_BASE(12, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(12, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jge(intptr_t a)
{
    assem_debug("bge %x", a);
    if(a < 4) {
        output_w32(BC_BASE(4, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(4, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jb(intptr_t a)
{
    assem_debug("blt(uns) %x", a);
    if(a < 4) {
        output_w32(BC_BASE(12, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(12, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jae(intptr_t a)
{
    assem_debug("bge(uns) %x", a);
    if(a < 4) {
        output_w32(BC_BASE(4, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(4, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_js(intptr_t a)
{
    assem_debug("blt(sign) %x", a);
    if(a < 4) {
        output_w32(BC_BASE(12, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(12, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jns(intptr_t a)
{
    assem_debug("bge(sign) %x", a);
    if(a < 4) {
        output_w32(BC_BASE(4, 0));
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(4, 0) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jno(intptr_t a)
{
    /* On PPC64 there is no overflow flag, so "no overflow" is
     * implemented as "branch if NOT KSEG0" after emit_cmpimm's andis.
     * test of bit 31:
     *   andis. r0, rs, 0x8000  → CR0[EQ]=1 if bit 31 clear (not KSEG0)
     *   beq stub                → branch to stub when NOT KSEG0
     * KSEG0 addresses (bit 31 set) fall through to the fast path. */
    assem_debug("beq (not KSEG0) %x", a);
    if(a < 4) {
        output_w32(BC_BASE(12, 2));  /* beq — branch if CR0[EQ]=1 */
    } else {
        intptr_t offset = a - (intptr_t)out;
        output_w32(BC_BASE(12, 2) | (((offset >> 2) & 0x3fff) << 2));
    }
}

static void emit_jno_unlikely(intptr_t a)
{
    emit_jno(a);
}

/* ======================================================================== */
/* External jumps — emit trampoline to absolute target                       */
/* ======================================================================== */
static void emit_extjump2(intptr_t addr, int target, intptr_t linker)
{
    assem_debug("extjump2 %x->%x linker=%p", addr, target, (void*)linker);
    /* dynamic_linker(void *src, u_int vaddr) expects:
     *   r3 = src (placeholder address in code buffer)
     *   r4 = vaddr (N64 target address)
     * The placeholder B instruction at addr will be patched by set_jump_target
     * to jump directly to the compiled block once available, bypassing this stub. */
    int ar1 = PPC_HREG(ARG1_REG);  /* r3 — src */
    int ar2 = PPC_HREG(ARG2_REG);  /* r4 — vaddr */
    emit_movimm64(addr, ar1);
    if((u_int)target < 65536) {
        emit_mov(target, ar2);
    } else {
        emit_movimm((u_int)target, ar2);
    }
    emit_jmp(linker);
}

/* ======================================================================== */
/* Conditional moves                                                         */
/* ======================================================================== */
/* PPC970 doesn't have isel; use bc+skip pattern for conditional moves */

static void emit_cmovne_reg(int hr, int addr)
{
    assem_debug("cmovne r%d, r%d", hr, addr);
    /* beq $+8  (skip mr if equal) */
    output_w32(BC_BASE(12, 2) | (2 << 2));
    emit_mov(addr, hr);
}

static void emit_cmovl_reg(int hr, int addr)
{
    assem_debug("cmovl r%d, r%d", hr, addr);
    /* bge $+8 (skip mr if not less) */
    output_w32(BC_BASE(4, 0) | (2 << 2));
    emit_mov(addr, hr);
}

static void emit_cmovs_reg(int hr, int addr)
{
    assem_debug("cmovs r%d, r%d", hr, addr);
    /* bge $+8 (skip mr if not negative) */
    output_w32(BC_BASE(4, 0) | (2 << 2));
    emit_mov(addr, hr);
}

static void emit_cmovs(int hr)
{
    assem_debug("cmovs(imm0) r%d", hr);
    /* bge $+8 (skip mr if not negative => value < 0 -> move zero) */
    output_w32(BC_BASE(4, 0) | (2 << 2));
    emit_zeroreg(hr);
}

static void emit_cmov2imm_e_ne_compact(int ba, int ha, int nt, int alt)
{
    assem_debug("cmov2imm %x,%x to r%d alt r%d", ba, ha, nt, alt);
    /* if equal: copy ha to nt, else keep ba */
    emit_jne((intptr_t)(out+8)); /* skip mov if not equal */
    emit_add(ha, 0, nt);
    /* alt is the alternative when branch is not taken,
       but we keep the value in the destination register */
}

/* ======================================================================== */
/* ALU: add + set flags, add w/carry, subtract w/borrow                     */
/* ======================================================================== */
static void emit_adds(int s1, int s2, int d)
{
    assem_debug("addc. r%d, r%d, r%d (sets CA+CR)", d, s1, s2);
    output_w32(X_FORM_HR(31, d, s1, s2, XO_ADDC, 1));
}

static void emit_addnop(int n)
{
    (void)n;
    output_w32(0x60000000);
}

static void emit_subs(int s1, int s2, int d)
{
    assem_debug("subf. r%d, r%d, r%d", d, s2, s1);
    output_w32(X_FORM_HR(31, d, s2, s1, 40, 1));
}

static void emit_adc(int s1, int s2, int d)
{
    assem_debug("adde r%d, r%d, r%d", d, s1, s2);
    output_w32(X_FORM_HR(31, d, s1, s2, 138, 0));
}

static void emit_adcimm(int imm, int d)
{
    assem_debug("addic r%d, r%d, %d", d, d, imm);
    output_w32(D_FORM_HR(12, d, d, imm));
}

static void emit_sbc(int s1, int s2, int d)
{
    assem_debug("subfe r%d, r%d, r%d", d, s2, s1);
    output_w32(X_FORM_HR(31, d, s2, s1, 136, 0));
}

static void emit_rscimm(int rs, int imm, int d)
{
    assert(imm == 0);
    assem_debug("subfze r%d, r%d", d, rs);
    output_w32(X_FORM(31, HREG(d), HREG(rs), 0, 200, 0));
}

static void emit_negs(int s, int d)
{
    assem_debug("neg. r%d, r%d", d, s);
    output_w32(X_FORM(31, HREG(d), HREG(s), 0, 104, 1));
}

static void emit_addl(int s1, int s2, int d)
{
    assem_debug("add r%d, r%d, r%d", d, s1, s2);
    output_w32(X_FORM_HR(31, d, s1, s2, 266, 0));
}

static void emit_addimm64_32(int sh, int sl, int imm, int th, int tl)
{
    assem_debug("addimm64_32 r%d:r%d += %d", th, tl, imm);
    if(imm >= 0) {
        emit_addimm(sl, imm, tl);
        emit_adc(sh, 0, th);
    } else {
        emit_addimm(sl, imm, tl);
        emit_adcimm(-1, th);
    }
}

/* ======================================================================== */
/* Immediate AND/OR/XOR                                                      */
/* ======================================================================== */
static void emit_andimm(int s, int imm, int d)
{
    assem_debug("andi r%d, r%d, %d", d, s, imm);
    if(imm >= -32768 && imm < 65536) {
        /* Use andi. or andis. */
        if(imm < 0) {
            output_w32(D_FORM_HR(OP_ANDIS, d, s, (unsigned short)imm));
        } else {
            output_w32(D_FORM_HR(OP_ANDI, d, s, imm));
        }
    } else {
        emit_movimm(imm, HOST_TEMPREG);
        emit_and(s, HOST_TEMPREG, d);
    }
}

static void emit_andimm64(int s, int64_t imm, int d)
{
    /* For PPC64, AND with 16-bit unsigned immediate (ANDI/ANDIS) */
    if(imm >= 0 && imm <= 0xFFFF) {
        output_w32(D_FORM_HR(OP_ANDI, d, s, (int)imm));
    } else if(imm >= 0xFFFF0000LL && imm <= 0xFFFFFFFFLL) {
        output_w32(D_FORM_HR(OP_ANDIS, d, s, (unsigned short)(imm >> 16)));
    } else {
        emit_movimm64(imm, HOST_TEMPREG);
        emit_and(s, HOST_TEMPREG, d);
    }
}

static void emit_orimm(int s, int imm, int d)
{
    assem_debug("ori r%d, r%d, %d", d, s, imm);
    if(imm >= 0 && imm <= 0xFFFF) {
        output_w32(D_FORM_HR(OP_ORI, d, s, imm));
    } else {
        output_w32(D_FORM_HR(OP_ORIS, d, s, (unsigned short)(imm >> 16)));
    }
}

static void emit_xorimm(int s, int imm, int d)
{
    assem_debug("xori r%d, r%d, %d", d, s, imm);
    if(imm >= 0 && imm <= 0xFFFF) {
        output_w32(D_FORM_HR(OP_XORI, d, s, imm));
    } else {
        output_w32(D_FORM_HR(OP_XORIS, d, s, (unsigned short)(imm >> 16)));
    }
}

/* ======================================================================== */
/* ALU: sub64_32 (64-bit host subtract for 32-bit MIPS)                     */
/* ======================================================================== */
static void emit_sub64_32(int s1, int s2, int d)
{
    assem_debug("subf r%d, r%d, r%d (64_32)", d, s2, s1);
    output_w32(X_FORM_HR(31, d, s2, s1, 40, 0));
}

/* ======================================================================== */
/* Set operations (_32 variants for NATIVE_64=1)                             */
/* ======================================================================== */
static void emit_set_nz32(int rs, int rt)
{
    assem_debug("set_nz32 r%d, r%d", rt, rs);
    emit_sltiu(rs, 1, rt);
}

static void emit_set_nz64_32(int rsh, int rsl, int rt)
{
    (void)rsh;
    emit_set_nz32(rsl, rt);
}

static void emit_set_gz32(int rs, int rt)
{
    assem_debug("set_gz32 r%d, r%d", rt, rs);
    emit_slti(rs, 1, rt);
}

static void emit_set_gz64_32(int rsh, int rsl, int rt)
{
    (void)rsh;
    emit_set_gz32(rsl, rt);
}

static void emit_set_if_carry64_32(int s1h, int s1l, int s2h, int s2l, int rt)
{
    (void)s1h; (void)s2h;
    emit_set_if_carry32(s1l, s2l, rt);
}

static void emit_set_if_less64_32(int s1h, int s1l, int s2h, int s2l, int rt)
{
    (void)s1h; (void)s2h;
    emit_set_if_less32(s1l, s2l, rt);
}

/* ======================================================================== */
/* 64-bit shift helpers (shldimm/shrdimm via rldicr/rldicl)                */
/* ======================================================================== */
static void emit_shldimm(int rsh, int rsl, int shift, int d)
{
    (void)rsh;
    if(shift == 0) {
        emit_mov(rsl, d);
    } else if(shift < 64) {
        emit_shlimm64(rsl, shift, d);
    } else {
        emit_zeroreg(d);
    }
}

static void emit_shrdimm(int rsl, int rsh, int shift, int d)
{
    (void)rsh;
    if(shift == 0) {
        emit_mov(rsl, d);
    } else if(shift < 64) {
        emit_shrimm64(rsl, shift, d);
    } else {
        emit_zeroreg(d);
    }
}

/* ======================================================================== */
/* Load/store: TLB variants, sign/zero extend                               */
/* ======================================================================== */

/* Read byte with TLB */
static void emit_movzbl_tlb(int addr, int map, int rt)
{
    assem_debug("movzbl_tlb addr=%d map=%d rt=%d", addr, map, rt);
    emit_readbyte_indexed_tlb(addr, 0, map, rt);
}

static void emit_movzbl_indexed_tlb(int addr, int rs, int map, int rt)
{
    assem_debug("movzbl_indexed_tlb rs=%d map=%d rt=%d", rs, map, rt);
    emit_readbyte_indexed_tlb(addr, rs, map, rt);
}

static void emit_movsbl_tlb(int addr, int map, int rt)
{
    assem_debug("movsbl_tlb addr=%d map=%d rt=%d", addr, map, rt);
    emit_readbyte_indexed_tlb(addr, 0, map, rt);
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 26, 0)); /* extsb rt, rt */
}

static void emit_movsbl_indexed_tlb(int addr, int rs, int map, int rt)
{
    assem_debug("movsbl_indexed_tlb rs=%d map=%d rt=%d", rs, map, rt);
    emit_readbyte_indexed_tlb(addr, rs, map, rt);
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 26, 0)); /* extsb rt, rt */
}

static void emit_movzwl_tlb(int addr, int map, int rt)
{
    assem_debug("movzwl_tlb addr=%d map=%d rt=%d", addr, map, rt);
    emit_readhword_indexed_tlb(addr, 0, map, rt);
}

static void emit_movzwl_indexed_tlb(int addr, int rs, int map, int rt)
{
    assem_debug("movzwl_indexed_tlb rs=%d map=%d rt=%d", rs, map, rt);
    emit_readhword_indexed_tlb(addr, rs, map, rt);
}

static void emit_movswl_tlb(int addr, int map, int rt)
{
    assem_debug("movswl_tlb addr=%d map=%d rt=%d", addr, map, rt);
    emit_readhword_indexed_tlb(addr, 0, map, rt);
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 58, 0)); /* extsh rt, rt */
}

static void emit_movswl_indexed_tlb(int addr, int rs, int map, int rt)
{
    assem_debug("movswl_indexed_tlb rs=%d map=%d rt=%d", rs, map, rt);
    emit_readhword_indexed_tlb(addr, rs, map, rt);
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 58, 0)); /* extsh rt, rt */
}

/* Absolute address sign/zero extend loads */
static void emit_movsbl(intptr_t addr, int rt)
{
    assem_debug("movsbl (abs) r%d <- %p", rt, (void*)addr);
    /* On PPC64 BE: uint64_t rdword stores byte at byte 7 (LSB).
     * Use offset from FP to load the LSB. */
    intptr_t offset = addr - (intptr_t)&g_dev.r4300.new_dynarec_hot_state;
    offset += 7; /* LSB of 8-byte rdword on BE */
    assert(offset < 0x8000);
    output_w32(D_FORM_HR(OP_LBZ, rt, FP, offset));
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 26, 0)); /* extsb rt, rt */
}

static void emit_movswl(intptr_t addr, int rt)
{
    assem_debug("movswl (abs) r%d <- %p", rt, (void*)addr);
    intptr_t offset = addr - (intptr_t)&g_dev.r4300.new_dynarec_hot_state;
    offset += 6; /* LSH (bytes 6-7) of 8-byte rdword on BE */
    assert(offset < 0x8000);
    assert((offset & 1) == 0);
    output_w32(D_FORM_HR(OP_LHZ, rt, FP, offset));
    output_w32(X_FORM(31, HREG(rt), HREG(rt), 0, 58, 0)); /* extsh rt, rt */
}

static void emit_movzbl(intptr_t addr, int rt)
{
    assem_debug("movzbl (abs) r%d <- %p", rt, (void*)addr);
    intptr_t offset = addr - (intptr_t)&g_dev.r4300.new_dynarec_hot_state;
    offset += 7; /* LSB of 8-byte rdword on BE */
    assert(offset < 0x8000);
    output_w32(D_FORM_HR(OP_LBZ, rt, FP, offset));
}

static void emit_movzwl(intptr_t addr, int rt)
{
    assem_debug("movzwl (abs) r%d <- %p", rt, (void*)addr);
    intptr_t offset = addr - (intptr_t)&g_dev.r4300.new_dynarec_hot_state;
    offset += 6; /* LSH (bytes 6-7) of 8-byte rdword on BE */
    assert(offset < 0x8000);
    assert((offset & 1) == 0);
    output_w32(D_FORM_HR(OP_LHZ, rt, FP, offset));
}

/* TLB-aware read dword */
static void emit_readdword_indexed_tlb(int addr, int rs, int map, int th, int tl)
{
    assem_debug("readdword_indexed_tlb addr=%d rs=%d map=%d", addr, rs, map);
    if(th >= 0 && th != tl) {
        emit_readword_indexed_tlb(addr, rs, map, th);
        emit_readword_indexed_tlb(addr+4, rs, map, tl);
    } else {
        /* NATIVE_64: combine both 32-bit reads into tl */
        emit_readword_indexed_tlb(addr+4, rs, map, tl);
        emit_readword_indexed_tlb(addr, rs, map, HOST_TEMPREG);
        emit_shlimm64(HOST_TEMPREG, 32, HOST_TEMPREG);
        emit_or(HOST_TEMPREG, tl, tl);
    }
}

static void emit_readdword_tlb(int addr, int map, int th, int tl)
{
    assem_debug("readdword_tlb addr=%d map=%d", addr, map);
    emit_readdword_indexed_tlb(addr, 0, map, th, tl);
}

static void emit_readword_tlb(int addr, int map, int rt)
{
    assem_debug("readword_tlb addr=%d map=%d rt=%d", addr, map, rt);
    emit_readword_indexed_tlb(addr, 0, map, rt);
}

/* TLB-aware dword store */
static void emit_writedword_indexed_tlb(int rh, int rl, int addr, int rs, int map)
{
    assem_debug("writedword_indexed_tlb rh=%d rl=%d addr=%d rs=%d map=%d", rh, rl, addr, rs, map);
    /* On PPC64 NATIVE_64, rh == rl (same register for high and low)
     * Extract high 32 bits and store at addr, low 32 bits at addr+4 */
    if(rh != rl) {
        emit_writeword_indexed_tlb(rh, addr, rs, map);
        emit_writeword_indexed_tlb(rl, addr+4, rs, map);
    } else {
        emit_sarimm64(rl, 32, HOST_TEMPREG);
        emit_writeword_indexed_tlb(HOST_TEMPREG, addr, rs, map);
        emit_writeword_indexed_tlb(rl, addr+4, rs, map);
    }
}

/* Immediate value store */
static void emit_writeword_imm(int imm, int addr)
{
    assem_debug("writeword_imm %d to (abs)%x", imm, addr);
    emit_movimm(imm, HOST_TEMPREG);
    emit_writeword(HOST_TEMPREG, addr);
}

/* ======================================================================== */
/* Memory comparison branches                                               */
/* ======================================================================== */
static void emit_cmpmem_imm(intptr_t addr, int imm)
{
    assem_debug("cmpmem_imm (%p) vs %d", (void*)addr, imm);
    emit_readword(addr, HOST_TEMPREG);
    emit_cmpimm(HOST_TEMPREG, imm);
}

static void emit_cmpmem_indexedsr12_imm(intptr_t addr, int r, int imm)
{
    assem_debug("cmpmem_indexedsr12_imm base=%p r%d imm=%d", (void*)addr, r, imm);
    emit_readword_indexed(0, r, HOST_TEMPREG);
    emit_cmpimm(HOST_TEMPREG, imm);
}

static void emit_cmpmem_indexedsr12_reg(int r1, int r2, int rt)
{
    assem_debug("cmpmem_indexedsr12_reg r%d(imm0) vs r%d", r1, r2);
    emit_readword_indexed(0, r1, HOST_TEMPREG);
    emit_cmp(HOST_TEMPREG, r2);
}

/* ======================================================================== */
/* Prefetch                                                                  */
/* ======================================================================== */
static void emit_prefetch(intptr_t addr)
{
    /* PPC970: dcbt (data cache block touch) */
    assem_debug("dcbt %p", (void*)addr);
    emit_movimm64(addr, HOST_TEMPREG);
    output_w32(X_FORM(31, 0, 0, HREG(HOST_TEMPREG), 278, 0)); /* dcbt 0, rTEMPREG */
}

static void emit_prefetchreg(int r)
{
    /* PPC970: dcbt (data cache block touch from register) */
    assem_debug("dcbt r%d", r);
    output_w32(X_FORM(31, 0, 0, HREG(r), 278, 0)); /* dcbt 0, rR */
}

/* ======================================================================== */
/* Register management                                                       */
/* ======================================================================== */
static void emit_loadreg(u_int r, int hr)
{
    assem_debug("loadreg r%d <- reg[%d]", hr, r);
    if((r & 63) == 0) {
        emit_zeroreg(hr);
    } else if(r == MMREG) {
        emit_movimm(fp_memory_map, hr);
    } else if(r == INVCP || r == ROREG) {
        u_int offset = 0;
        if(r == INVCP) offset = fp_invc_ptr;
        if(r == ROREG) offset = fp_ram_offset;
        assert(offset < 0x8000);
        assert((offset & 3) == 0);
        emit_readdword_indexed(offset, FP, hr);  /* 64-bit load — intptr_t fields */
    } else {
        u_int offset;
        if((r & 63) == HIREG)
            offset = fp_hi;
        else if((r & 63) == LOREG)
            offset = fp_lo;
        else
            offset = fp_regs + ((r & 63) << 3);
        if(r == CCREG) {
            offset = fp_cycle_count;
            assert(offset < 0x8000);
            assert((offset & 3) == 0);
            emit_readword_indexed(offset, FP, hr);
        } else if(r == CSREG) {
            offset = fp_cp0_regs(CP0_STATUS_REG);
            assert(offset < 0x8000);
            assert((offset & 3) == 0);
            emit_readword_indexed(offset, FP, hr);
        } else if(r == FSREG) {
            offset = fp_fcr31;
            assert(offset < 0x8000);
            assert((offset & 3) == 0);
            emit_readword_indexed(offset, FP, hr);
        } else {
            /* PPC64 BE: low 32 bits at +4, high 32 bits at +0 within 8-byte slot */
            if(r & 64) {
                /* high part */
                offset += 0;
            } else {
                /* low part */
                offset += 4;
            }
            assert(offset < 0x8000);
            assert((offset & 3) == 0);
            emit_readword_indexed(offset, FP, hr);
        }
    }
}

static void emit_storereg(int r, int hr)
{
    assem_debug("storereg r%d -> reg[%d]", hr, r);
    u_int offset;
    if((r & 63) == HIREG)
        offset = fp_hi;
    else if((r & 63) == LOREG)
        offset = fp_lo;
    else
        offset = fp_regs + ((r & 63) << 3);
    if(r == CCREG) {
        offset = fp_cycle_count;
    } else if(r == FSREG) {
        offset = fp_fcr31;
    } else {
        /* PPC64 BE: low 32 bits at +4, high 32 bits at +0 within 8-byte slot */
        if(r & 64) {
            /* high part */
            offset += 0;
        } else {
            /* low part */
            offset += 4;
        }
    }
    assert((r & 63) != CSREG);
    assert((r & 63) != 0);
    assert((r & 63) <= CCREG);
    assert(offset < 0x8000);
    assert((offset & 3) == 0);
    emit_writeword_indexed(hr, offset, FP);
}

static void emit_pushimm(int imm)
{
    assem_debug("pushimm %d", imm);
    /* PPC: stwu (store word with update) pushes onto stack */
    emit_movimm(imm, HOST_TEMPREG);
    output_w32(D_FORM(OP_STWU, HREG(HOST_TEMPREG), SP, -4));
}

static void emit_readptr(intptr_t addr, int rt)
{
    assem_debug("readptr %p -> r%d", (void*)addr, rt);
    emit_movimm64(addr, HOST_TEMPREG);
    emit_readdword_indexed(0, HOST_TEMPREG, rt);
}

/* ======================================================================== */
/* 64-bit call via mtctr + bctrl (unlimited range)                           */
/* ======================================================================== */
static void emit_64bit_call(intptr_t addr, int scratch)
{
    assem_debug("emit_64bit_call %p (scratch=r%d)", (void*)addr, scratch);
    /* Load target address into scratch register */
    emit_movimm64(addr, scratch);
    /* mtctr scratch: mtspr 9, scratch */
    /* X_FORM(31, rs, spr[4:0], spr[9:5], 467, 0) = mtspr */
    /* 9/0 are CTR SPR number, NOT register indices — use HREG only on scratch */
    output_w32(X_FORM(31, HREG(scratch), 9, 0, XO_MTSPR, 0));
    /* bcctrl 20,0 (branch always to CTR with link) */
    output_w32(0x4E800421);
}

/* ======================================================================== */
/* Call (conditional)                                                        */
/* ======================================================================== */
static void emit_callne(intptr_t addr)
{
    assem_debug("callne %p", (void*)addr);
    if(addr < 4) return;
    emit_64bit_call(addr, HOST_TEMPREG);
}

/* ======================================================================== */
/* mov64 (64-bit move for register remapping)                               */
/* ======================================================================== */
static void emit_mov64(int hr, int nr)
{
    assem_debug("mov64 r%d, r%d", nr, hr);
    emit_mov(hr, nr);
}

/* ======================================================================== */
/* mov2imm_compact — move two immediates to registers if condition matches  */
/* ======================================================================== */
static void emit_mov2imm_compact(int imm1, int r1, int imm2, int r2)
{
    assem_debug("mov2imm_compact %x->r%d, %x->r%d", imm1, r1, imm2, r2);
    emit_movimm(imm1, r1);
    emit_movimm(imm2, r2);
}

/* ======================================================================== */
/* ADD with sl2 (shift left by 2) — used for address calculation             */
/* ======================================================================== */
static void emit_addsl2(int rs, int ra, int rd)
{
    assem_debug("addsl2 r%d, r%d, r%d", rd, rs, ra);
    /* PPC: rldicr rd, rs, 2, 61  (rotate left 2, clear upper 61 bits = shift left 2 in 64-bit) */
    emit_shlimm64(rs, 2, HOST_TEMPREG);
    emit_add(ra, HOST_TEMPREG, rd);
}

/* ======================================================================== */
/* slt / sltu variants for _32 and _64_32                                    */
/* ======================================================================== */
static void emit_slti32(int rs, int imm, int rt)
{
    emit_slti(rs, imm, rt);
}

static void emit_sltiu32(int rs, int imm, int rt)
{
    emit_sltiu(rs, imm, rt);
}

static void emit_slti64_32(int rsh, int rsl, int imm, int rt)
{
    (void)rsh;
    emit_slti(rsl, imm, rt);
}

static void emit_sltiu64_32(int rsh, int rsl, int imm, int rt)
{
    (void)rsh;
    emit_sltiu(rsl, imm, rt);
}

/* ======================================================================== */
/* save_regs / restore_regs — save/restore caller-saved host regs           */
/* ======================================================================== */
static void save_regs(u_int reglist)
{
    /* PPC64 caller-saved: r3-r12 = host indices 0-9
     * Use 64-bit stores (std) because registers hold full 64-bit values
     * (pointers, ram_offset, memory_map entries). 32-bit stw would truncate. */
    reglist &= 0x3FF;
    if(!reglist) return;
    int i, count = 0;
    int regs[10];
    for(i = 0; i < 10; i++) {
        if(reglist & (1 << i))
            regs[count++] = host_reg_ppc[i];
    }
    if(!count) return;
    int stack_size = ((count * 8) + 15) & ~15;
    /* Allocate stack frame: addi SP, SP, -stack_size (raw PPC numbers) */
    output_w32(D_FORM(OP_ADDI, SP, SP, -stack_size));
    /* Save each register (64-bit) */
    int offset = 0;
    for(i = 0; i < count; i++) {
        output_w32(D_FORM(OP_STD, regs[i], SP, offset));
        offset += 8;
    }
}

static void restore_regs(u_int reglist)
{
    reglist &= 0x3FF;
    if(!reglist) return;
    int i, count = 0;
    int regs[10];
    for(i = 0; i < 10; i++) {
        if(reglist & (1 << i))
            regs[count++] = host_reg_ppc[i];
    }
    if(!count) return;
    int stack_size = ((count * 8) + 15) & ~15;
    /* Restore registers (64-bit) */
    int offset = 0;
    for(i = 0; i < count; i++) {
        output_w32(D_FORM(OP_LD, regs[i], SP, offset));
        offset += 8;
    }
    /* Deallocate stack frame: addi SP, SP, stack_size (raw PPC numbers) */
    output_w32(D_FORM(OP_ADDI, SP, SP, stack_size));
}

/* ======================================================================== */
/* literal_pool / literal_pool_jumpover                                      */
/* ======================================================================== */
static void literal_pool(int n)
{
    if((!literalcount)||(n!=0)) return;
    u_int *ptr;
    int i;
    for(i = 0; i < literalcount; i++)
    {
        ptr = (u_int *)literals[i][0];
        intptr_t offset = (intptr_t)out - (intptr_t)ptr;
        assert(offset >= -33554432LL && offset < 33554432LL);
        assert((offset & 3) == 0);
        *ptr |= (((offset >> 2) & 0x3ffffff) << 2);
        output_w64(literals[i][1]);
    }
    literalcount = 0;
}

static void literal_pool_jumpover(int n)
{
    (void)n;
}

/* ======================================================================== */
/* set_rounding_mode — set FPSCR rounding mode from MIPS FCR31              */
/* ======================================================================== */
static void set_rounding_mode(int s, int temp)
{
    assem_debug("set_rounding_mode s=r%d temp=r%d", s, temp);
    assert(temp >= 0);
    /* Load rounding mode from rounding_modes[s & 3] */
    emit_andimm(s, 3, temp);
    emit_shlimm(temp, 2, temp);
    emit_addimm64(FP, fp_rounding_modes, HOST_TEMPREG);
    emit_readword_dualindexed(HOST_TEMPREG, temp, temp);
    
    /* Read current FPSCR into FPR0, store to stack, modify RN field */
    output_w32(0xFC00000E);      /* mffs f0 */
    output_w32(D_FORM(54, 0, SP, -8)); /* stfd f0, -8(SP) */
    output_w32(D_FORM(OP_LWZ, HREG(HOST_TEMPREG), SP, -8));  /* lwz rHOST_TEMPREG, -8(SP) */
    emit_andimm(HOST_TEMPREG, 0x3FFFFFFF, HOST_TEMPREG); /* clear RN bits 30-31 */
    emit_or(temp, HOST_TEMPREG, HOST_TEMPREG);
    output_w32(D_FORM(OP_STW, HREG(HOST_TEMPREG), SP, -8));  /* stw rHOST_TEMPREG, -8(SP) */
    output_w32(D_FORM(50, 0, SP, -8)); /* lfd f0, -8(SP) */
    output_w32(0xFC00058E);      /* mtfsf f0 */
}

/* ======================================================================== */
/* generate_map_const — load memory_map entry address                        */
/* ======================================================================== */
static void generate_map_const(u_int addr, int tr)
{
    assem_debug("generate_map_const %x -> r%d", addr, tr);
    /* Compute byte offset into memory_map array: (addr>>12)*8 = addr>>9 */
    uintptr_t const_val = ((uintptr_t)(addr >> 12) * 8) + fp_memory_map;
    fprintf(stderr, "PPC64: generate_map_const addr=0x%x const_val=0x%lx fp_memory_map=0x%lx\n",
            addr, (unsigned long)const_val, (unsigned long)fp_memory_map);
    emit_movimm64(const_val, tr);
}

/* ======================================================================== */
/* TLB helper functions (ported from ARM64)                                  */
/* ======================================================================== */

/* Back-patch targets for the dynamic KSEG0 fast path jump.
 * Set by do_tlb_r/do_tlb_w when they emit a KSEG0 bypass for c=0,
 * cleared by do_tlb_r_branch/do_tlb_w_branch after fixing up the target. */
static intptr_t kseg0_read_jmp = 0;
static intptr_t kseg0_write_jmp = 0;

static int do_tlb_r(int s, int ar, int map, int cache, int x, int c, u_int addr)
{
    (void)ar;
    if(c) {
        if((signed int)addr >= (signed int)0xC0000000) {
            emit_readdword_dualindexed(FP, map, map);
        }
        else if((signed int)addr < (signed int)0x80800000) {
            emit_loadreg(ROREG, HOST_TEMPREG);
            return HOST_TEMPREG;
        }
        else
            return -1;
    }
    else {
        assert(s != map);
        /* KSEG0 fast path for dynamic addresses.
         * KSEG0 = 0x80000000-0x9FFFFFFF (bits 31=1, 30=0, 29=0).
         * Test: bit 31 set AND bits 30-29 both clear.
         * KSEG1 (0xA0000000+) and KSEG2 (0xC0000000+) must use memory_map. */
        kseg0_read_jmp = 0;
        /* andis. r0, s, 0x8000 → r0 = s & 0x80000000, sets CR0[EQ] */
        output_w32(D_FORM(OP_ANDIS, 0, HREG(s), 0x8000));
        intptr_t jnokseg = (intptr_t)out;
        emit_jeq(0);  /* branch to memory_map if bit 31 clear */

        /* andis. r0, s, 0x6000 → r0 = s & 0x60000000 (bits 30+29 combined) */
        output_w32(D_FORM(OP_ANDIS, 0, HREG(s), 0x6000));
        intptr_t jkseg12 = (intptr_t)out;
        emit_jne(0);  /* branch to memory_map if bit 30 or 29 set (KSEG1/KSEG2) */

        /* KSEG0: map = ram_offset  (fast path — addr added by indexed load) */
        emit_loadreg(ROREG, map);

        /* Jump over the memory_map code + do_tlb_r_branch code */
        kseg0_read_jmp = (intptr_t)out;
        emit_jmp(0);

        /* Not KSEG0: fall through to memory_map path */
        set_jump_target(jnokseg, (intptr_t)out);
        set_jump_target(jkseg12, (intptr_t)out);

        if(cache >= 0) {
            /* Use cached offset to memory map */
            emit_shrimm(s, 9, HOST_TEMPREG);
            emit_add(cache, HOST_TEMPREG, map);
        } else {
            emit_loadreg(MMREG, map);
            emit_shrimm(s, 9, HOST_TEMPREG);
            emit_add(map, HOST_TEMPREG, map);
        }
        emit_readdword_dualindexed(FP, map, map);
    }
    return map;
}

static int do_tlb_r_branch(int map, int c, u_int addr, intptr_t *jaddr)
{
    if(!c || (signed int)addr >= (signed int)0xC0000000) {
        emit_test(map, map);
        *jaddr = (intptr_t)out;
        emit_js(0);
        /* memory_map stores (host_ptr - 0x80000000) >> 2 to fit in 32 bits.
         * PPC indexed loads (lwzx) do not scale the index, so we must
         * re-shift to get the full byte offset before using as base. */
        emit_shlimm64(map, 2, map);
    }

    /* Fix up KSEG0 fast path jump target to skip over the TLB branch+shift code.
     * The KSEG0 fast path (ram_offset + addr) does not need shifting. */
    if(kseg0_read_jmp) {
        set_jump_target(kseg0_read_jmp, (intptr_t)out);
        kseg0_read_jmp = 0;
    }
    return map;
}

static int do_tlb_w(int s, int ar, int map, int cache, int x, int c, u_int addr)
{
    (void)ar;
    if(c) {
        if(addr < 0x80800000 || addr >= 0xC0000000) {
            emit_readdword_dualindexed(FP, map, map);
        }
        else
            return -1;
    }
    else {
        assert(s != map);
        /* KSEG0 fast path for dynamic addresses (same as do_tlb_r) */
        kseg0_write_jmp = 0;
        /* andis. r0, s, 0x8000 → r0 = s & 0x80000000, sets CR0[EQ] */
        output_w32(D_FORM(OP_ANDIS, 0, HREG(s), 0x8000));
        intptr_t jnokseg0 = (intptr_t)out;
        emit_jeq(0);

        /* KSEG0: map = ram_offset (addr added by indexed store) */
        emit_loadreg(ROREG, map);

        kseg0_write_jmp = (intptr_t)out;
        emit_jmp(0);

        set_jump_target(jnokseg0, (intptr_t)out);

        if(cache >= 0) {
            emit_shrimm(s, 9, HOST_TEMPREG);
            emit_add(cache, HOST_TEMPREG, map);
        } else {
            emit_loadreg(MMREG, map);
            emit_shrimm(s, 9, HOST_TEMPREG);
            emit_add(map, HOST_TEMPREG, map);
        }
        emit_readdword_dualindexed(FP, map, map);
    }
    return map;
}

static void do_tlb_w_branch(int map, int c, u_int addr, intptr_t *jaddr)
{
    if(!c || addr < 0x80800000 || addr >= 0xC0000000) {
        /* WRITE_PROTECT is 64-bit, use emit_movimm64 + emit_test */
        emit_movimm64(WRITE_PROTECT, HOST_TEMPREG);
        emit_test(map, HOST_TEMPREG);
        *jaddr = (intptr_t)out;
        emit_jne(0);
        /* Shift memory_map value to get full byte offset for indexed loads.
         * Must happen after WRITE_PROTECT check which uses pre-shifted value. */
        emit_shlimm64(map, 2, map);
    }

    /* Fix up KSEG0 fast path jump target (same as do_tlb_r_branch) */
    if(kseg0_write_jmp) {
        set_jump_target(kseg0_write_jmp, (intptr_t)out);
        kseg0_write_jmp = 0;
    }
}

/* ======================================================================== */
/* do_invstub — invalidate code stub                                         */
/* ======================================================================== */
static void do_invstub(int n)
{
    if(stubs[n][4] == -1) return;
    literal_pool(20);
    u_int reglist = stubs[n][3];
    set_jump_target(stubs[n][1], (intptr_t)out);
    save_regs(reglist);
    if(stubs[n][4] != 0) emit_mov(stubs[n][4], PPC_HREG(ARG1_REG));
    emit_call((intptr_t)&invalidate_addr);
    restore_regs(reglist);
    emit_jmp(stubs[n][2]);
}

/* ======================================================================== */
/* do_dirty_stub / do_dirty_stub_ds — code verification stubs               */
/* ======================================================================== */
static intptr_t do_dirty_stub(int i, struct ll_entry *head)
{
    assem_debug("do_dirty_stub %x", head->vaddr);
    /* Load head pointer into r3 (ARG1_REG) and call verify_code. */
    emit_movimm64((uintptr_t)head, PPC_HREG(ARG1_REG));
    emit_call((intptr_t)verify_code);
    intptr_t entry = (intptr_t)out;
    load_regs_entry(i);
    if(entry == (intptr_t)out) entry = instr_addr[i];
    emit_jmp(instr_addr[i]);
    return entry;
}

static void do_dirty_stub_ds(struct ll_entry *head)
{
    assem_debug("do_dirty_stub_ds %x", head->vaddr);
    emit_movimm64((uintptr_t)head, PPC_HREG(ARG1_REG));
    emit_call((intptr_t)verify_code);
}

/* ======================================================================== */
/* do_clear_cache — flush icache/dcache for recompiled blocks               */
/* ======================================================================== */
static void do_clear_cache(void)
{
    int i, j;
    for(i = 0; i < (1 << (TARGET_SIZE_2 - 17)); i++)
    {
        u_int bitmap = needs_clear_cache[i];
        if(bitmap) {
            uintptr_t start, end;
            for(j = 0; j < 32; j++)
            {
                if(bitmap & (1 << j)) {
                    start = (intptr_t)base_addr_rx + i * 131072 + j * 4096;
                    end = start + 4095;
                    j++;
                    while(j < 32) {
                        if(bitmap & (1 << j)) {
                            end += 4096;
                            j++;
                        } else {
                            cache_flush((char *)start, (char *)end);
                            break;
                        }
                    }
                }
            }
            needs_clear_cache[i] = 0;
        }
    }
}

/* ======================================================================== */
/* Mini-Hash Table helpers (port from ARM64)                                 */
/* ======================================================================== */
static void do_preload_rhash(int r)
{
    (void)r;
}

static void do_preload_rhtbl(int ht)
{
    emit_addimm64(FP, fp_mini_ht, ht);
}

static void do_rhash(int rs, int rh)
{
    emit_andimm(rs, 0x1F0, rh);
}

static void do_miniht_load(int ht, int rh)
{
    emit_add(ht, rh, ht);
    emit_readword_indexed(0, ht, rh);
}

static void do_miniht_jump(int rs, int rh, int ht)
{
    emit_cmp(rh, rs);
    intptr_t jaddr = (intptr_t)out;
    emit_jeq(0);
    emit_jmp(jump_vaddr_reg[rs]);
    set_jump_target(jaddr, (intptr_t)out);
    emit_readdword_indexed(8, ht, ht);
    output_w32(0x7C6903A6); /* mtctr ht */
    output_w32(0x4E800420); /* bctr */
}

static void do_miniht_insert(u_int return_address, int rt, int temp)
{
    emit_movimm64(return_address, rt);
    /* Compute address of mini_ht entry[rt->next_temp] */
    emit_movimm64((intptr_t)out, temp);
    emit_writedword(temp, (intptr_t)&g_dev.r4300.new_dynarec_hot_state.mini_ht[(return_address & 0x1FF) >> 4][1]);
    emit_writeword(rt, (intptr_t)&g_dev.r4300.new_dynarec_hot_state.mini_ht[(return_address & 0x1FF) >> 4][0]);
}

/* ======================================================================== */
/* FPU emitter helpers — native PPC floating-point instruction output       */
/* ======================================================================== */

/* FP A-form: opcode(6) | frt(5) | fra(5) | frb(5) | xo(5) | xo2(5) | rc(1) */
/* On PPC, the A-form extended opcode is 10 bits but encoded as xo(5)+0(5)  */
/* The XO_FORM / X_FORM macros put rt/ra/rb at the correct bit positions    */

/* Double-precision FP arithmetic (opcode 63) */
static void emit_fadd(int frd, int fra, int frb)
{
    output_w32(XO_FORM(63, frd, fra, frb, XO_FADD, 0));
}
static void emit_fsub(int frd, int fra, int frb)
{
    output_w32(XO_FORM(63, frd, fra, frb, XO_FSUB, 0));
}
static void emit_fmul(int frd, int fra, int frb)
{
    output_w32(XO_FORM(63, frd, fra, frb, XO_FMUL, 0));
}
static void emit_fdiv(int frd, int fra, int frb)
{
    output_w32(XO_FORM(63, frd, fra, frb, XO_FDIV, 0));
}

/* Single-precision FP arithmetic (opcode 59) */
static void emit_fadds(int frd, int fra, int frb)
{
    output_w32(XO_FORM(59, frd, fra, frb, XO_FADD, 0));
}
static void emit_fsubs(int frd, int fra, int frb)
{
    output_w32(XO_FORM(59, frd, fra, frb, XO_FSUB, 0));
}
static void emit_fmuls(int frd, int fra, int frb)
{
    output_w32(XO_FORM(59, frd, fra, frb, XO_FMUL, 0));
}
static void emit_fdivs(int frd, int fra, int frb)
{
    output_w32(XO_FORM(59, frd, fra, frb, XO_FDIV, 0));
}

/* FP X-form unary: opcode(6) | frt(5) | ra(5)| rb(5) | xo(10) | rc(1)    */
/* For unary ops: frb is at ra position (bits 20-16), rb=0                 */
static void emit_fsqrt(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FSQRT, 0));
}
static void emit_fsqrts(int frt, int frb)
{
    output_w32(X_FORM(59, frt, frb, 0, XO_FSQRT, 0));
}
static void emit_fabs(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FABS, 0));
}
static void emit_fmr(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FMR, 0));
}
static void emit_fneg(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FNEG, 0));
}

/* Conversion & FPSCR */
static void emit_fctiwz(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FCTIWZ, 0));
}
static void emit_fctiw(int frt, int frb)
{
    output_w32(X_FORM(63, frt, frb, 0, XO_FCTIW, 0));
}
static void emit_frsp(int frt, int frb)
{
    /* frsp — round double to single precision */
    output_w32(X_FORM(63, frt, frb, 0, XO_FRSP, 0));
}
static void emit_fcfid(int frt, int frb)
{
    /* fcfid — convert int64 in frb to double in frt */
    output_w32(X_FORM(63, frt, frb, 0, XO_FCFID, 0));
}
static void emit_mffs(int frt)
{
    /* mffs frt — move FPSCR to FPR frt */
    output_w32(X_FORM(63, frt, 0, 0, XO_MFFS, 0));
}
static void emit_mtfsfi(int bf, int imm)
{
    /* mtfsfi bf,imm — set FPSCR field BF to 4-bit IMM */
    /* Encoding: (63<<26) | (bf<<23) | (0<<21) | (imm<<16) | (0<<11) | (134<<1) | 0 */
    output_w32((63 << 26) | ((bf) << 23) | ((imm) << 16) | (XO_MTFSFI << 1));
}

/* FP loads/stores — D-form with FPR (frt/frs is raw FPR number) */
static void emit_lfs(int frt, int offset, int ra)
{
    output_w32(D_FORM(OP_LFS, frt, ra, offset));
}
static void emit_lfd(int frt, int offset, int ra)
{
    output_w32(D_FORM(OP_LFD, frt, ra, offset));
}
static void emit_stfs(int frs, int offset, int ra)
{
    output_w32(D_FORM(OP_STFS, frs, ra, offset));
}
static void emit_stfd(int frs, int offset, int ra)
{
    output_w32(D_FORM(OP_STFD, frs, ra, offset));
}

/* STFIWX: store FPR as 32-bit integer, opcode=31, X-form */
/* X_FORM(31, frs, ra, rb, 983, 0) */
static void emit_stfiwx(int frs, int ra, int rb)
{
    output_w32(X_FORM(31, frs, ra, rb, XO_STFIWX, 0));
}

/* FCMPU: FP compare unordered, sets CR field crfd */
/* Encoding: X_FORM(63, crfd<<2, fra, frb, 0, 0) */
static void emit_fcmpu(int crfd, int fra, int frb)
{
    output_w32(X_FORM(63, crfd << 2, fra, frb, XO_FCMPU, 0));
}

/* ======================================================================== */
/* fconv_assemble — FPU conversion & rounding ops                           */
/* ======================================================================== */
#define fconv_assemble fconv_assemble_ppc64
static void fconv_assemble_ppc64(int i, struct regstat *i_regs)
{
    signed char temp = get_reg(i_regs->regmap, -1);
    assert(temp >= 0);
    if(!cop1_usable) {
        signed char cs = get_reg(i_regs->regmap, CSREG);
        assert(cs >= 0);
        emit_testimm(cs, CP0_STATUS_CU1);
        intptr_t jaddr = (intptr_t)out;
        emit_jeq(0);
        add_stub(FP_STUB, jaddr, (intptr_t)out, i, cs, (intptr_t)i_regs, is_delayslot, 0);
        cop1_usable = 1;
    }
    u_int hr, reglist = 0;
    for(hr = 0; hr < HOST_REGS; hr++) {
        if(i_regs->regmap[hr] >= 0) reglist |= 1 << hr;
    }
    save_regs(reglist);

    int fs = (source[i] >> 11) & 0x1f;
    int fd = (source[i] >> 6) & 0x1f;
    int op = source[i] & 0x3f;
    int op2 = opcode2[i];

    /* ================================================================== */
    /* Native PPC conversions                                              */
    /* ================================================================== */

    /* cvt_s_w: op2=0x14, op=0x20  (int32 -> float) — no direct PPC insn */
    /* cvt_d_w: op2=0x14, op=0x21  (int32 -> double) — no direct PPC insn */
    /* cvt_s_l: op2=0x15, op=0x20  (int64 -> float) — C fallback */
    /* cvt_d_l: op2=0x15, op=0x21  (int64 -> double) — C fallback */

    /* cvt_d_s: op2=0x10, op=0x21  (float -> double): lfs auto-extends to double */
    if(op2 == 0x10 && op == 0x21) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
        emit_lfs(FPR_SCR0, 0, HREG(temp));
        if(fs != fd)
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], temp);
        emit_stfd(FPR_SCR0, 0, HREG(temp));
        return;
    }

    /* cvt_s_d: op2=0x11, op=0x20  (double -> float) */
    if(op2 == 0x11 && op == 0x20) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
        emit_lfd(FPR_SCR0, 0, HREG(temp));
        emit_frsp(FPR_SCR0, FPR_SCR0);
        if(fs != fd)
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
        emit_stfs(FPR_SCR0, 0, HREG(temp));
        return;
    }

    /* cvt_w_s: op2=0x10, op=0x24  (float -> int32, truncation) */
    if(op2 == 0x10 && op == 0x24) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
        emit_lfs(FPR_SCR0, 0, HREG(temp));
        emit_fctiwz(FPR_SCR0, FPR_SCR0);
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], HOST_TEMPREG);
        emit_stfiwx(FPR_SCR0, 0, HREG(HOST_TEMPREG));
        return;
    }

    /* cvt_w_d: op2=0x11, op=0x24  (double -> int32, truncation) */
    if(op2 == 0x11 && op == 0x24) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
        emit_lfd(FPR_SCR0, 0, HREG(temp));
        emit_fctiwz(FPR_SCR0, FPR_SCR0);
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], HOST_TEMPREG);
        emit_stfiwx(FPR_SCR0, 0, HREG(HOST_TEMPREG));
        return;
    }

    /* Single-precision rounding: TRUNC_W_S (op2=0x10, op=0x0d) */
    if(op2 == 0x10 && op == 0x0d) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
        emit_lfs(FPR_SCR0, 0, HREG(temp));
        emit_fctiwz(FPR_SCR0, FPR_SCR0);
        if(fs != fd)
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
        emit_stfiwx(FPR_SCR0, 0, HREG(temp));
        return;
    }

    /* Double-precision rounding: TRUNC_W_D (op2=0x11, op=0x0d) */
    if(op2 == 0x11 && op == 0x0d) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
        emit_lfd(FPR_SCR0, 0, HREG(temp));
        emit_fctiwz(FPR_SCR0, FPR_SCR0);
        if(fs != fd)
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
        emit_stfiwx(FPR_SCR0, 0, HREG(temp));
        return;
    }

    /* ================================================================== */
    /* C fallbacks for int-to-float, int64, rounding variants              */
    /* ================================================================== */

    reglist = 0;
    for(hr = 0; hr < HOST_REGS; hr++) {
        if(i_regs->regmap[hr] >= 0) reglist |= 1 << hr;
    }
    save_regs(reglist);

    /* cvt_s_w: op2=0x14, op=0x20  (int32 -> float) */
    if(op2 == 0x14 && op == 0x20) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_s_w);
    }
    /* cvt_d_w: op2=0x14, op=0x21  (int32 -> double) */
    if(op2 == 0x14 && op == 0x21) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_d_w);
    }
    /* cvt_s_l: op2=0x15, op=0x20  (int64 -> float) */
    if(op2 == 0x15 && op == 0x20) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_s_l);
    }
    /* cvt_d_l: op2=0x15, op=0x21  (int64 -> double) */
    if(op2 == 0x15 && op == 0x21) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_d_l);
    }
    /* cvt_l_s: op2=0x10, op=0x25  (float -> int64) */
    if(op2 == 0x10 && op == 0x25) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_l_s);
    }
    /* cvt_l_d: op2=0x11, op=0x25  (double -> int64) */
    if(op2 == 0x11 && op == 0x25) {
        emit_addimm64(FP, fp_fcr31, PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG2_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG3_REG));
        emit_call((intptr_t)cvt_l_d);
    }

    /* Single-precision rounding variants (non-truncation) */
    if(op2 == 0x10 && op == 0x08) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)round_l_s);
    }
    if(op2 == 0x10 && op == 0x09) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)trunc_l_s);
    }
    if(op2 == 0x10 && op == 0x0a) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)ceil_l_s);
    }
    if(op2 == 0x10 && op == 0x0b) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)floor_l_s);
    }
    if(op2 == 0x10 && op == 0x0c) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)round_w_s);
    }
    if(op2 == 0x10 && op == 0x0e) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)ceil_w_s);
    }
    if(op2 == 0x10 && op == 0x0f) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)floor_w_s);
    }

    /* Double-precision rounding variants (non-truncation) */
    if(op2 == 0x11 && op == 0x08) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)round_l_d);
    }
    if(op2 == 0x11 && op == 0x09) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)trunc_l_d);
    }
    if(op2 == 0x11 && op == 0x0a) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)ceil_l_d);
    }
    if(op2 == 0x11 && op == 0x0b) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)floor_l_d);
    }
    if(op2 == 0x11 && op == 0x0c) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)round_w_d);
    }
    if(op2 == 0x11 && op == 0x0e) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)ceil_w_d);
    }
    if(op2 == 0x11 && op == 0x0f) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], PPC_HREG(ARG1_REG));
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], PPC_HREG(ARG2_REG));
        emit_call((intptr_t)floor_w_d);
    }

    restore_regs(reglist);
}

/* ======================================================================== */
/* float_assemble — FPU arithmetic ops via native PPC FP instructions       */
/* ======================================================================== */
static void float_assemble(int i, struct regstat *i_regs)
{
    signed char temp = get_reg(i_regs->regmap, -1);
    assert(temp >= 0);
    if(!cop1_usable) {
        signed char cs = get_reg(i_regs->regmap, CSREG);
        assert(cs >= 0);
        emit_testimm(cs, CP0_STATUS_CU1);
        intptr_t jaddr = (intptr_t)out;
        emit_jeq(0);
        add_stub(FP_STUB, jaddr, (intptr_t)out, i, cs, (intptr_t)i_regs, is_delayslot, 0);
        cop1_usable = 1;
    }

    int fs = (source[i] >> 11) & 0x1f;
    int ft = (source[i] >> 16) & 0x1f;
    int fd = (source[i] >> 6) & 0x1f;
    int op = source[i] & 0x3f;
    int op2 = opcode2[i];

    /* Single precision (op2=0x10, op=0x00-0x07) */
    if(op2 == 0x10 && (op & ~7) == 0) {
        if(op >= 0 && op <= 3) {
            /* 3-arg: add/sub/mul/div */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[ft], HOST_TEMPREG);
            emit_lfs(FPR_SCR0, 0, HREG(temp));
            emit_lfs(FPR_SCR1, 0, HREG(HOST_TEMPREG));
            switch(op) {
            case 0: emit_fadds(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 1: emit_fsubs(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 2: emit_fmuls(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 3: emit_fdivs(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            }
            if(fs != fd && ft != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
            emit_stfs(FPR_SCR2, 0, HREG(temp));
        } else if(op == 4) {
            /* sqrt_s */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
            emit_lfs(FPR_SCR0, 0, HREG(temp));
            emit_fsqrts(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
            emit_stfs(FPR_SCR0, 0, HREG(temp));
        } else if(op == 5) {
            /* abs_s */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
            emit_lfs(FPR_SCR0, 0, HREG(temp));
            emit_fabs(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
            emit_stfs(FPR_SCR0, 0, HREG(temp));
        } else if(op == 6) {
            /* mov_s */
            if(fs != fd) {
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], HOST_TEMPREG);
                emit_lfs(FPR_SCR0, 0, HREG(temp));
                emit_stfs(FPR_SCR0, 0, HREG(HOST_TEMPREG));
            }
        } else if(op == 7) {
            /* neg_s */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
            emit_lfs(FPR_SCR0, 0, HREG(temp));
            emit_fneg(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fd], temp);
            emit_stfs(FPR_SCR0, 0, HREG(temp));
        }
        return;
    }

    /* Double precision (op2=0x11, op=0x00-0x07) */
    if(op2 == 0x11 && (op & ~7) == 0) {
        if(op >= 0 && op <= 3) {
            /* 3-arg: add/sub/mul/div */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[ft], HOST_TEMPREG);
            emit_lfd(FPR_SCR0, 0, HREG(temp));
            emit_lfd(FPR_SCR1, 0, HREG(HOST_TEMPREG));
            switch(op) {
            case 0: emit_fadd(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 1: emit_fsub(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 2: emit_fmul(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            case 3: emit_fdiv(FPR_SCR2, FPR_SCR0, FPR_SCR1); break;
            }
            if(fs != fd && ft != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], temp);
            emit_stfd(FPR_SCR2, 0, HREG(temp));
        } else if(op == 4) {
            /* sqrt_d */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
            emit_lfd(FPR_SCR0, 0, HREG(temp));
            emit_fsqrt(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], temp);
            emit_stfd(FPR_SCR0, 0, HREG(temp));
        } else if(op == 5) {
            /* abs_d */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
            emit_lfd(FPR_SCR0, 0, HREG(temp));
            emit_fabs(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], temp);
            emit_stfd(FPR_SCR0, 0, HREG(temp));
        } else if(op == 6) {
            /* mov_d */
            if(fs != fd) {
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], HOST_TEMPREG);
                emit_lfd(FPR_SCR0, 0, HREG(temp));
                emit_stfd(FPR_SCR0, 0, HREG(HOST_TEMPREG));
            }
        } else if(op == 7) {
            /* neg_d */
            emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
            emit_lfd(FPR_SCR0, 0, HREG(temp));
            emit_fneg(FPR_SCR0, FPR_SCR0);
            if(fs != fd)
                emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fd], temp);
            emit_stfd(FPR_SCR0, 0, HREG(temp));
        }
        return;
    }
}

/* ======================================================================== */
/* fcomp_assemble — FPU compare ops via native PPC fcmpu                     */
/* ======================================================================== */
static void fcomp_assemble(int i, struct regstat *i_regs)
{
    signed char temp = get_reg(i_regs->regmap, -1);
    assert(temp >= 0);
    if(!cop1_usable) {
        signed char cs = get_reg(i_regs->regmap, CSREG);
        assert(cs >= 0);
        emit_testimm(cs, CP0_STATUS_CU1);
        intptr_t jaddr = (intptr_t)out;
        emit_jeq(0);
        add_stub(FP_STUB, jaddr, (intptr_t)out, i, cs, (intptr_t)i_regs, is_delayslot, 0);
        cop1_usable = 1;
    }
    signed char fs_host = get_reg(i_regs->regmap, FSREG);
    assert(fs_host >= 0);

    int fs = (source[i] >> 11) & 0x1f;
    int ft = (source[i] >> 16) & 0x1f;
    int op = source[i] & 0x3f;
    int op2 = opcode2[i];

    /* ================================================================== */
    /* Native PPC fcmpu — single & double precision                        */
    /* ================================================================== */

    /* CR0 bit masks in mfcr result (CR0 at bits 28-31 of GPR):
     *   LT = 0x80000000 (bit 31)  — ordered less than
     *   GT = 0x40000000 (bit 30)  — ordered greater than
     *   EQ = 0x20000000 (bit 29)  — ordered equal
     *   SO = 0x10000000 (bit 28)  — unordered (NaN)
     * Tests use emit_testimm(HOST_TEMPREG, mask) which sets CR0[EQ]=1 if mask==0.
     * bc BT_BO(12), 2 (=EQ), +2 skips the OR when mask result is zero (condition false).
     */

    /* Always-false ops: c_f (0x30), c_sf (0x38) — just clear bit 23 */
    if(op == 0x30 || op == 0x38) {
        emit_andimm(fs_host, ~0x800000, fs_host);
        return;
    }

    /* Compute CR0 bitmask for the MIPS condition */
    u_int cr_mask;
    switch(op) {
    case 0x31: case 0x39: /* c_un, c_ngle: unordered (SO) */
        cr_mask = 0x10000000; break;
    case 0x32: case 0x3a: /* c_eq, c_seq: equal (EQ) */
        cr_mask = 0x20000000; break;
    case 0x33: case 0x3b: /* c_ueq, c_ngl: EQ|SO */
        cr_mask = 0x30000000; break;
    case 0x34: case 0x3c: /* c_olt, c_lt: LT */
        cr_mask = 0x80000000; break;
    case 0x35: case 0x3d: /* c_ult, c_nge: LT|SO */
        cr_mask = 0x90000000; break;
    case 0x36: case 0x3e: /* c_ole, c_le: LT|EQ */
        cr_mask = 0xA0000000; break;
    case 0x37: case 0x3f: /* c_ule, c_ngt: LT|EQ|SO */
        cr_mask = 0xB0000000; break;
    default:
        cr_mask = 0; break;
    }

    /* Load operands into FPR scratch registers */
    if(op2 == 0x10) {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[fs], temp);
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[ft], HOST_TEMPREG);
        emit_lfs(FPR_SCR0, 0, HREG(temp));
        emit_lfs(FPR_SCR1, 0, HREG(HOST_TEMPREG));
    } else {
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[fs], temp);
        emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[ft], HOST_TEMPREG);
        emit_lfd(FPR_SCR0, 0, HREG(temp));
        emit_lfd(FPR_SCR1, 0, HREG(HOST_TEMPREG));
    }

    /* Clear fcr31 condition flag bit 23, then compare and conditionally set */
    emit_andimm(fs_host, ~0x800000, fs_host);
    emit_fcmpu(0, FPR_SCR0, FPR_SCR1);
    /* Get CR into GPR — mfcr r12 (HOST_TEMPREG) */
    output_w32(X_FORM(OP_MFXSR, HREG(HOST_TEMPREG), 0, 0, XO_MFCR, 0));

    /* Test the condition bits, skip OR if none set */
    emit_testimm(HOST_TEMPREG, cr_mask);
    output_w32(B_FORM(OP_BC, BT_BO, 2, 2, 0, 0)); /* bc 12, 2, +8: skip if EQ=1 (mask==0) */
    emit_orimm(fs_host, 0x800000, fs_host);
}

/* ======================================================================== */
/* invalidate_addr — invalidate code block in TLB/cache                      */
/* ======================================================================== */
static void invalidate_addr(u_int addr)
{
    invalidate_block(addr>>12);
}

/* ======================================================================== */
/* emit_addnop — alignment NOP for 8-byte alignment                          */
/* ======================================================================== */

/* ======================================================================== */
/* Debug helper — called from linkage_ppc64.S before first bctr             */
/* ======================================================================== */
void ppc64_debug_jump(void *target)
{
    fprintf(stderr, "PPC64: jumping to %p\n", target);
}

#pragma GCC diagnostic pop
