/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - linkage_ppc64.S                                          *
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

#define GLOBAL_FUNCTION(name)  \
    .globl name;               \
    .hidden name;              \
    .type name, @function;     \
    name

#define LOCAL_FUNCTION(name)  \
    .hidden name;             \
    .type name, @function;    \
    name

#define GLOBAL_VARIABLE(name, size_) \
    .global name;                    \
    .hidden name;                    \
    .type   name, @object;           \
    .size   name, size_

#define TEXT_SECTION .text
#define END_SECTION

#include "asm_defines_gas.h"

/* Offset from g_dev to new_dynarec_hot_state (which starts with dynarec_local) */
#define HOT_STATE_OFFSET (offsetof_struct_device_r4300 + offsetof_struct_r4300_core_new_dynarec_hot_state)

/* Saved context: callee-save registers r14-r31 stored at end of dynarec_local[32] */
#define SAVED_CONTEXT     (32*8 - 18*8)

/* Frame pointer (r31) offset to hot_state fields (from start of hot_state) */
#define fp_cycle_count        (offsetof_struct_new_dynarec_hot_state_cycle_count)
#define fp_pending_exception  (offsetof_struct_new_dynarec_hot_state_pending_exception)
#define fp_pcaddr             (offsetof_struct_new_dynarec_hot_state_pcaddr)
#define fp_stop               (offsetof_struct_new_dynarec_hot_state_stop)

TEXT_SECTION

/* jump_vaddr_xN: move register N to r3, fall through to jump_vaddr */
GLOBAL_FUNCTION(jump_vaddr_r0):
    mr     3, 0
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r3):
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r4):
    mr     3, 4
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r5):
    mr     3, 5
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r6):
    mr     3, 6
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r7):
    mr     3, 7
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r8):
    mr     3, 8
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r9):
    mr     3, 9
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r10):
    mr     3, 10
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r11):
    mr     3, 11
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r12):
    mr     3, 12
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r14):
    mr     3, 14
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r16):
    mr     3, 16
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r17):
    mr     3, 17
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r18):
    mr     3, 18
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r19):
    mr     3, 19
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r20):
    mr     3, 20
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r21):
    mr     3, 21
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r22):
    mr     3, 22
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r23):
    mr     3, 23
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r24):
    mr     3, 24
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r25):
    mr     3, 25
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r26):
    mr     3, 26
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r27):
    mr     3, 27
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r28):
    mr     3, 28
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r29):
    mr     3, 29
    b      jump_vaddr

GLOBAL_FUNCTION(jump_vaddr_r30):
    mr     3, 30
    b      jump_vaddr

/* jump_vaddr: call get_addr_ht(r3), branch to returned address */
GLOBAL_FUNCTION(jump_vaddr):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    bl     get_addr_ht
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* verify_code: check if block is dirty, recompile if needed */
GLOBAL_FUNCTION(verify_code):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    std    14, 112(1)
    mr     14, 3
    bl     verify_dirty
    nop
    cmpdi  3, 0
    bne    .L1
    ld     14, 112(1)
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    blr
.L1:
    mr     3, 14
    bl     get_addr
    nop
    ld     14, 112(1)
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* cc_interrupt: periodic cycle count / interrupt check */
/* r14 = HOST_CCREG (cycle count) */
GLOBAL_FUNCTION(cc_interrupt):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    stw    14, fp_cycle_count(31)
    li     0, 0
    stw    0, fp_pending_exception(31)
    bl     dynarec_gen_interrupt
    nop
    lwz    14, fp_cycle_count(31)
    lwz    4, fp_pending_exception(31)
    lwz    5, fp_stop(31)
    cmpwi  5, 0
    bne    new_dyna_stop
    cmpwi  4, 0
    bne    .L2
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    blr
.L2:
    lwz    3, fp_pcaddr(31)
    bl     get_addr_ht
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* do_interrupt: exception dispatch */
GLOBAL_FUNCTION(do_interrupt):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    lwz    5, fp_stop(31)
    cmpwi  5, 0
    bne    new_dyna_stop
    lwz    3, fp_pcaddr(31)
    bl     get_addr_ht
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* fp_exception: cop1 unusable handler */
GLOBAL_FUNCTION(fp_exception):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    stw    3, fp_pcaddr(31)
    bl     cop1_unusable
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* jump_syscall: SYSCALL instruction handler */
GLOBAL_FUNCTION(jump_syscall):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    stw    3, fp_pcaddr(31)
    bl     SYSCALL_new
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* jump_eret: ERET instruction handler */
GLOBAL_FUNCTION(jump_eret):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    stw    14, fp_cycle_count(31)
    bl     ERET_new
    nop
    lwz    14, fp_cycle_count(31)
    cmpdi  3, 0
    beq    new_dyna_stop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* dyna_linker: dynamic block linking */
GLOBAL_FUNCTION(dyna_linker):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    bl     dynamic_linker
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* dyna_linker_ds: dynamic block linking (delay slot) */
GLOBAL_FUNCTION(dyna_linker_ds):
    mflr   0
    std    0, 16(1)
    stdu   1, -128(1)
    bl     dynamic_linker_ds
    nop
    ld     0, 144(1)
    addi   1, 1, 128
    mtlr   0
    mtctr  3
    bctr

/* new_dyna_stop: restore callee-saves and return to C */
LOCAL_FUNCTION(new_dyna_stop):
    addi   11, 31, (32*8 - 18*8)  /* r11 = &dynarec_local[SAVED_CONTEXT/8] */
    ld     14, 0(11)
    ld     15, 8(11)
    ld     16, 16(11)
    ld     17, 24(11)
    ld     18, 32(11)
    ld     19, 40(11)
    ld     20, 48(11)
    ld     21, 56(11)
    ld     22, 64(11)
    ld     23, 72(11)
    ld     24, 80(11)
    ld     25, 88(11)
    ld     26, 96(11)
    ld     27, 104(11)
    ld     28, 112(11)
    ld     29, 120(11)
    ld     30, 128(11)
    ld     31, 136(11)
    ld     0, 16(1)
    mtlr   0
    blr

/* new_dyna_start: entry point from C.
 * On entry: r1 = stack, r2 = TOC, r3-r10 = args
 * Saves callee-saves, sets r31 (FP) = &hot_state, calls new_recompile_block(0xA4000040) */
GLOBAL_FUNCTION(new_dyna_start):
    mflr   0
    std    0, 16(1)
    stdu   1, -256(1)
    /* Load hot_state address via TOC/GOT */
    addis  11, 2, g_dev@got@ha
    ld     11, g_dev@got@l(11)         /* r11 = &g_dev */
    lis    12, HOT_STATE_OFFSET@h
    ori    12, 12, HOT_STATE_OFFSET@l
    add    31, 11, 12                  /* r31 = &hot_state */
    /* Save callee-saves (r14-r31) into dynarec_local[SAVED_CONTEXT/8 .. 31] */
    addi   11, 31, SAVED_CONTEXT
    std    14, 0(11)
    std    15, 8(11)
    std    16, 16(11)
    std    17, 24(11)
    std    18, 32(11)
    std    19, 40(11)
    std    20, 48(11)
    std    21, 56(11)
    std    22, 64(11)
    std    23, 72(11)
    std    24, 80(11)
    std    25, 88(11)
    std    26, 96(11)
    std    27, 104(11)
    std    28, 112(11)
    std    29, 120(11)
    std    30, 128(11)
    std    31, 136(11)
    /* Load base_addr_rx */
    addis  15, 2, base_addr_rx@got@ha
    ld     15, base_addr_rx@got@l(15)
    ld     15, 0(15)                   /* r15 = HOST_BTREG */
    /* Call new_recompile_block(0xA4000040) */
    lis    3, 0xA400
    ori    3, 3, 0x0040
    bl     new_recompile_block
    nop
    /* Reload base_addr_rx fresh from GOT — r15 may be clobbered by C code
     * even though it is nominally callee-saved (the generaged code keeps
     * r15 as HOST_BTREG and the register allocator may allocate it). */
    addis  11, 2, base_addr_rx@got@ha
    ld     11, base_addr_rx@got@l(11)
    ld     11, 0(11)
    /* Diagnostic: save r11, print jump target, restore r11 */
    std    11, 176(1)
    mr     3, 11
    bl     ppc64_debug_jump
    nop
    ld     11, 176(1)
    /* Load cycle_count */
    lwz    14, fp_cycle_count(31)      /* r14 = HOST_CCREG */
    /* Jump to first block */
    mtctr  11
    bctr

GLOBAL_FUNCTION(breakpoint):
    trap
    blr
