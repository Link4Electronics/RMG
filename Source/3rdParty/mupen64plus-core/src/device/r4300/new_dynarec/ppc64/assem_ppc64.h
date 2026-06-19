#ifndef M64P_DEVICE_R4300_NEW_DYNAREC_PPC64_ASSEM_PPC64_H
#define M64P_DEVICE_R4300_NEW_DYNAREC_PPC64_ASSEM_PPC64_H

/* PPC64 ELFv2 register usage in generated code:
 *   r3-r10: argument/return registers (caller-save, all allocatable)
 *   r11-r12: caller-save scratch (allocatable)
 *   r1: stack pointer (EXCLUDED from alloc)
 *   r2: TOC pointer (EXCLUDED from alloc, preserved for C calls)
 *   r13: thread pointer (EXCLUDED from alloc)
 *   r14: HOST_CCREG - cycle count (callee-save, allocated)
 *   r15: HOST_BTREG - base address (callee-save, allocated)
 *   r16-r30: general purpose (callee-save)
 *   r31: FP - frame pointer, points to hot_state->dynarec_local
 *   NOTE: r0 is NOT allocatable — writes are discarded and reads return 0 in D-form
 */

#define HOST_REGS 27
#define HOST_CCREG 10   /* host_reg_ppc index for r14 */
#define HOST_BTREG 11   /* host_reg_ppc index for r15 */
#define EXCLUDE_REG HOST_REGS

#define HOST_TEMPREG 9   /* host_reg_ppc index for r12 */

#define NATIVE_64 1
#define RAM_OFFSET 1
#define USE_MINI_HT 1

#define TARGET_SIZE_2 25
#define JUMP_TABLE_ENTRY_SIZE 32
#define JUMP_TABLE_SIZE 8192

/* PPC64 ELFv2 calling convention */
#define ARG1_REG 3
#define ARG2_REG 4
#define ARG3_REG 5
#define ARG4_REG 6

#define FP 31
#define SP 1
#define LR 0
#define CALLER_SAVED_REGS 0x3FF

/* CR field for condition codes */
#define CR_ALU 0
#define CR_FPU 1

#endif /* M64P_DEVICE_R4300_NEW_DYNAREC_PPC64_ASSEM_PPC64_H */
