#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "device/r4300/r4300_core.h"
#include "device/r4300/cp0.h"
#include "device/rdram/rdram.h"
#include "ppc_dynarec_compat.h"
#include "Recompile.h"
#include "Recomp-Cache.h"
#include "Wrappers.h"
#include "Register-Cache.h"
#include "MIPS-to-PPC.h"

/* Global variables for instruction streaming (defined in MIPS-to-PPC.c) */
extern MIPS_instr* src_ptr_global;
extern PowerPC_instr* dst_ptr_global;
extern unsigned int src_pc_val;
extern int set_next_dst_override_val;
static PowerPC_instr code_buffer[64*1024];
static PowerPC_instr* code_addr_buffer[1024];
static unsigned char isJmpDst[1024];

static PowerPC_func* cf;
static jump_node jump_table[MAX_JUMPS];
static unsigned int current_jump;

void reset_code_addr(void)
{
    unsigned int idx = (src_pc_val - 4 - cf->start_address) >> 2;
    if(idx < 1024)
        code_addr_buffer[idx] = dst_ptr_global;
}

void nop_ignored(void)
{
    unsigned int idx = (src_pc_val - 4 - cf->start_address) >> 2;
    if(idx < 1024)
        code_addr_buffer[idx] = dst_ptr_global;
}

int is_j_out(int branch, int is_aa)
{
    if(is_aa)
        return ((branch << 2 | (cf->start_address & 0xF0000000)) <  cf->start_address ||
                (branch << 2 | (cf->start_address & 0xF0000000)) >= cf->end_address);
    else {
        int dst_instr = ((src_pc_val - cf->start_address - 4) >> 2) + branch;
        return (dst_instr < 0 || dst_instr >= (int)((cf->end_address - cf->start_address) >> 2));
    }
}

int is_j_dst(void)
{
    return isJmpDst[(get_src_pc() & 0xfff) >> 2];
}

static int pass0(PowerPC_block* ppc_block, PowerPC_func* func)
{
    int i;
    for(i = 0; i < 1024; ++i) isJmpDst[i] = 0;

    MIPS_instr* src = (MIPS_instr*)(ppc_dynarec_r4300->rdram->dram +
        ((func->start_address & 0x1FFFFFFF) >> 2));
    unsigned int vaddr = func->start_address;

    while(vaddr < func->end_address) {
        MIPS_instr mips = *src++;
        int opcode = MIPS_GET_OPCODE(mips);
        int index = (vaddr - ppc_block->start_address) >> 2;

        if(opcode == MIPS_OPCODE_J || opcode == MIPS_OPCODE_JAL) {
            unsigned int li = MIPS_GET_LI(mips);
            unsigned int jump_dst = (li << 2) | (vaddr & 0xF0000000);
            int out = (jump_dst < func->start_address || jump_dst >= func->end_address);
            if(!out)
                isJmpDst[li & 0x3FF] = 1;
            if(opcode == MIPS_OPCODE_JAL && index + 2 < 1024)
                isJmpDst[index + 2] = 1;
            src++; vaddr += 4; /* skip delay slot */
            if(opcode == MIPS_OPCODE_J) break;
        } else if(opcode == MIPS_OPCODE_BEQ   ||
                  opcode == MIPS_OPCODE_BNE   ||
                  opcode == MIPS_OPCODE_BLEZ  ||
                  opcode == MIPS_OPCODE_BGTZ  ||
                  opcode == MIPS_OPCODE_BEQL  ||
                  opcode == MIPS_OPCODE_BNEL  ||
                  opcode == MIPS_OPCODE_BLEZL ||
                  opcode == MIPS_OPCODE_BGTZL ||
                  opcode == MIPS_OPCODE_B     ||
                  (opcode == MIPS_OPCODE_COP1 &&
                   MIPS_GET_RS(mips) == MIPS_FRMT_BC)) {
            int bd = MIPS_GET_IMMED(mips);
            bd = (bd & 0x8000) ? (bd | 0xFFFF0000) : bd; /* sign extend */
            int dst_index = index + 1 + bd;
            if(dst_index >= 0 && dst_index < 1024)
                isJmpDst[dst_index] = 1;
            if(index + 2 < 1024)
                isJmpDst[index + 2] = 1;
            src++; vaddr += 4; /* skip delay slot */
        } else if(opcode == MIPS_OPCODE_R &&
                  (MIPS_GET_FUNC(mips) == MIPS_FUNC_JR ||
                   MIPS_GET_FUNC(mips) == MIPS_FUNC_JALR)) {
            src++; vaddr += 4; /* skip delay slot */
            break;
        } else if(opcode == MIPS_OPCODE_COP0 &&
                  MIPS_GET_FUNC(mips) == MIPS_FUNC_ERET) {
            break;
        }
        vaddr += 4;
    }

    if(vaddr < func->end_address) {
        func->end_address = vaddr;
        return 0;
    }
    return 1;
}

static void pass2(PowerPC_block* ppc_block, PowerPC_func* func)
{
    int i;
    for(i = 0; i < (int)current_jump; ++i) {
        PowerPC_instr* current = jump_table[i].dst_instr;

        if(jump_table[i].type & JUMP_TYPE_SPEC) {
            if(!(jump_table[i].type & JUMP_TYPE_J)) {
                *current &= ~(PPC_BD_MASK << PPC_BD_SHIFT);
                PPC_SET_BD(*current, jump_table[i].new_jump);
            } else {
                *current &= ~(PPC_LI_MASK << PPC_LI_SHIFT);
                PPC_SET_LI(*current, jump_table[i].new_jump);
            }
            continue;
        }

        if(jump_table[i].type & JUMP_TYPE_CALL) {
            int jump_offset = ((uintptr_t)jump_table[i].old_jump -
                               (uintptr_t)current) / 4;
            *current &= ~(PPC_LI_MASK << PPC_LI_SHIFT);
            PPC_SET_LI(*current, jump_offset);
        } else if(!(jump_table[i].type & JUMP_TYPE_J)) {
            int jump_offset = (unsigned int)jump_table[i].old_jump +
                     ((jump_table[i].src_pc - func->start_address) >> 2);
            jump_table[i].new_jump = func->code_addr[jump_offset] - current;
            *current &= ~(PPC_LI_MASK << PPC_LI_SHIFT);
            PPC_SET_LI(*current, jump_table[i].new_jump);
        } else {
            unsigned int jump_addr = (jump_table[i].old_jump << 2) |
                                     (ppc_block->start_address & 0xF0000000);
            int jump_offset = (jump_addr - func->start_address) >> 2;
            jump_table[i].new_jump = func->code_addr[jump_offset] - current;
            *current &= ~(PPC_LI_MASK << PPC_LI_SHIFT);
            PPC_SET_LI(*current, jump_table[i].new_jump);
        }
    }
}

static void genJumpPad(void)
{
    /* Load full 64-bit address of noCheckInterrupt on PPC64 */
    uint64_t nci_addr = (uint64_t)(&noCheckInterrupt);
    PowerPC_instr tmp;
    EMIT_LIS(3, (nci_addr >> 48) & 0xFFFF);
    EMIT_ORI(3, 3, (nci_addr >> 32) & 0xFFFF);
    GEN_RLDICR(tmp, 3, 3, 32, 31, 0); set_next_dst(tmp);
    EMIT_ORIS(3, 3, (nci_addr >> 16) & 0xFFFF);
    EMIT_ORI(3, 3, nci_addr & 0xFFFF);
    EMIT_LI(0, 1);
    EMIT_STW(0, 0, 3);
    EMIT_LIS(3, (get_src_pc() + 4) >> 16);
    EMIT_ORI(3, 3, get_src_pc() + 4);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_MTLR(0);
    EMIT_BLR(0);
}

PowerPC_func* recompile_block(PowerPC_block* ppc_block, unsigned int addr)
{
    ppc_block->adler32 = 0;

    PowerPC_func* func = calloc(1, sizeof(PowerPC_func));
    cf = func;

    func->start_address = addr;
    func->end_address = ppc_block->end_address;
    func->magic = FUNC_MAGIC;
    func->code = NULL;
    func->code_length = 0;
    func->links_in = NULL;
    func->links_out = NULL;
    func->lru = 0;

    /* Debug: dump first 16 MIPS words at the source address */
    if (ppc_dynarec_r4300 && ppc_dynarec_r4300->rdram) {
        MIPS_instr* dbg_src = (MIPS_instr*)(ppc_dynarec_r4300->rdram->dram + ((addr & 0x1FFFFFFF) >> 2));
        fprintf(stderr, "[RECOMP] reading MIPS code from rdram+0x%08X (vaddr 0x%08X), first words:",
                (addr & 0x1FFFFFFF), addr);
        int di;
        for (di = 0; di < 16 && di < 64; di++)
            fprintf(stderr, " %08X", dbg_src[di]);
        fprintf(stderr, "\n");
    }

    int need_pad = pass0(ppc_block, func);

    invalidate_func(func->end_address - 4);
    insert_func(&ppc_block->funcs, func);

    cf = func;

    src_ptr_global = (MIPS_instr*)(ppc_dynarec_r4300->rdram->dram +
        ((addr & 0x1FFFFFFF) >> 2));
    src_pc_val = addr;
    dst_ptr_global = code_buffer;
    set_next_dst_override_val = 0;
    current_jump = 0;
    memset(code_addr_buffer, 0, sizeof(code_addr_buffer));

    start_new_block();
    isJmpDst[(src_pc_val - ppc_block->start_address) >> 2] = 1;

    int i;
    for(i = 0; i < 1024; ++i) {
        if(ppc_block->flags[i] & BLOCK_FLAG_SPLIT)
            isJmpDst[i] = 1;
    }

    need_pad |= isJmpDst[(func->end_address - 4 - ppc_block->start_address) >> 2];

    while(src_pc_val < func->end_address) {
        unsigned int offset = (src_pc_val - ppc_block->start_address) >> 2;
        if(isJmpDst[offset]) {
            src_pc_val += 4;
            start_new_mapping();
            src_pc_val -= 4;
        }
        convert();
    }

    flushRegisters();

    if(need_pad)
        genJumpPad();

    func->code_length = (unsigned int)(dst_ptr_global - code_buffer);

    if(func->code_length == 0) {
        free(func);
        return NULL;
    }

    RecompCache_Alloc(func->code_length * sizeof(PowerPC_instr), addr, func);

    memcpy(func->code, code_buffer, func->code_length * sizeof(PowerPC_instr));
    unsigned int num_instrs = (func->end_address - func->start_address + 4) >> 2;
    unsigned int copy_size = (num_instrs < 1024) ? num_instrs : 1024;
    memcpy(func->code_addr, code_addr_buffer, copy_size * sizeof(PowerPC_instr*));

    /* Adjust code_addr pointers from compile buffer to final buffer */
    int idx;
    for(idx = 0; idx < (int)copy_size; ++idx)
        if(func->code_addr[idx])
            func->code_addr[idx] = func->code + (func->code_addr[idx] - code_buffer);

    /* Adjust jump table pointers from compile buffer to final buffer */
    for(idx = 0; idx < (int)current_jump; ++idx)
        jump_table[idx].dst_instr = func->code +
            (jump_table[idx].dst_instr - code_buffer);

    /* Fix up jump/branch instructions */
    pass2(ppc_block, func);

    DCFlushRange(func->code, func->code_length * sizeof(PowerPC_instr));
    ICInvalidateRange(func->code, func->code_length * sizeof(PowerPC_instr));

    {
        unsigned int* code32 = (unsigned int*)func->code;
        fprintf(stderr, "[RECOMP] final code at %p: first 4 instrs = %08X %08X %08X %08X\n",
                func->code, code32[0], code32[1], code32[2], code32[3]);
    }

    return func;
}

int mips_is_jump(MIPS_instr instr)
{
    int opcode = MIPS_GET_OPCODE(instr);
    int format = MIPS_GET_RS(instr);
    int func   = MIPS_GET_FUNC(instr);
    return (opcode == MIPS_OPCODE_J     ||
            opcode == MIPS_OPCODE_JAL   ||
            opcode == MIPS_OPCODE_BEQ   ||
            opcode == MIPS_OPCODE_BNE   ||
            opcode == MIPS_OPCODE_BLEZ  ||
            opcode == MIPS_OPCODE_BGTZ  ||
            opcode == MIPS_OPCODE_BEQL  ||
            opcode == MIPS_OPCODE_BNEL  ||
            opcode == MIPS_OPCODE_BLEZL ||
            opcode == MIPS_OPCODE_BGTZL ||
            opcode == MIPS_OPCODE_B     ||
            (opcode == MIPS_OPCODE_R    &&
             (func  == MIPS_FUNC_JR     ||
              func  == MIPS_FUNC_JALR)) ||
            (opcode == MIPS_OPCODE_COP1 &&
             format == MIPS_FRMT_BC));
}

int add_jump(int old_jump, int is_j, int is_call)
{
    if(current_jump >= MAX_JUMPS) return -1;
    int id = current_jump;
    jump_node* jump = &jump_table[current_jump++];
    jump->old_jump  = old_jump;
    jump->new_jump  = 0;
    jump->src_pc    = src_pc_val - 4;
    jump->dst_instr = dst_ptr_global;
    jump->type      = (is_j    ? JUMP_TYPE_J    : 0)
                    | (is_call ? JUMP_TYPE_CALL : 0);
    return id;
}

int add_jump_special(int is_j)
{
    if(current_jump >= MAX_JUMPS) return -1;
    int id = current_jump;
    jump_node* jump = &jump_table[current_jump++];
    jump->new_jump  = 0;
    jump->dst_instr = dst_ptr_global;
    jump->type      = JUMP_TYPE_SPEC | (is_j ? JUMP_TYPE_J : 0);
    return id;
}

void set_jump_special(int which, int new_jump)
{
    if(which < (int)current_jump && which >= 0) {
        jump_node* jump = &jump_table[which];
        if(!(jump->type & JUMP_TYPE_SPEC)) return;
        jump->new_jump = new_jump;
    }
}

PowerPC_block* blocks_get(unsigned int idx)
{
    if(idx < 0x100000)
        return blocks[idx];
    return NULL;
}

void init_block(PowerPC_block* ppc_block)
{
    if(ppc_block) {
        memset(ppc_block->flags, 0, sizeof(ppc_block->flags));
        ppc_block->funcs = NULL;
    }
    invalidateRegisters();
}

void deinit_block(PowerPC_block* ppc_block)
{
    if(ppc_block) {
        if(ppc_block->funcs) {
            PowerPC_func_node* node = ppc_block->funcs;
            remove_outgoing_links(&node, NULL);
        }
        ppc_block->funcs = NULL;
    }
}

void invalidate_block(PowerPC_block* ppc_block)
{
    invalid_code[ppc_block->start_address >> 12] = 0;
    if(ppc_block->funcs) {
        PowerPC_func_node* node = ppc_block->funcs;
        remove_outgoing_links(&node, NULL);
    }
    ppc_block->funcs = NULL;
    memset(ppc_block->flags, 0, sizeof(ppc_block->flags));
}

char txtbuffer[1024];
int do_disasm = 0;

int disassemble(unsigned int a, unsigned int op)
{
    (void)a; (void)op;
    return 0;
}
