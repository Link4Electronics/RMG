#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "device/r4300/r4300_core.h"
#include "device/r4300/cp0.h"
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

PowerPC_func* recompile_block(PowerPC_block* ppc_block, unsigned int addr){
    int i;
    PowerPC_func* func;
    int jump_index;
    int max_instructions = 0x1000 / 4;
    int num_jumps = 0;

    func = MetaCache_Alloc(sizeof(PowerPC_func));
    memset(func, 0, sizeof(PowerPC_func));
    func->start_address = addr;
    func->end_address = addr + 4;
    func->magic = FUNC_MAGIC;
    func->code = NULL;
    func->code_length = 0;
    func->links_in = NULL;
    func->links_out = NULL;
    func->lru = 0;

    start_new_block();
    src_ptr_global = (MIPS_instr*)(ppc_dynarec_r4300->ops->rdram() + (addr & 0x1FFFFFFF));
    src_pc_val = addr;
    dst_ptr_global = (PowerPC_instr*)func->code;
    set_next_dst_override_val = 0;

    for(i = 0; i < max_instructions; i++){
        instr = get_next_src();
        if(MIPS_GET_OPCODE(instr) == 0 && MIPS_GET_FUNC(instr) == 0){
            /* NOP or similar */
            EMIT_ORI(0, 0, 0, 0);
            continue;
        }

        if(mips_is_jump(instr)){
            jump_index = add_jump(num_jumps,
                MIPS_GET_OPCODE(instr) == MIPS_OPCODE_J ||
                MIPS_GET_OPCODE(instr) == MIPS_OPCODE_JAL,
                MIPS_GET_OPCODE(instr) == MIPS_OPCODE_JAL ||
                (MIPS_GET_OPCODE(instr) == MIPS_OPCODE_R &&
                 MIPS_GET_FUNC(instr) == MIPS_FUNC_JALR));

            if(jump_index < 0) break;

            ppc_block->flags[(addr - ppc_block->start_address)>>2] = 1;
            func->end_address = src_pc_val;
            break;
        }

        if(convert() == CONVERT_ERROR){
            printf("PPC dynarec: convert error at 0x%08x, falling back\n", src_pc_val-4);
            break;
        }

        if(ppc_block->flags[(src_pc_val - ppc_block->start_address)>>2] & BLOCK_FLAG_SPLIT){
            func->end_address = src_pc_val;
            break;
        }

        if(num_jumps > MAX_JUMPS){
            printf("PPC dynarec: too many jumps\n");
            break;
        }
    }

    func->code_length = (unsigned int)((unsigned long)dst_ptr_global - (unsigned long)func->code);

    if(func->code_length == 0){
        MetaCache_Free(func);
        return NULL;
    }

    RecompCache_Alloc(func->code_length, addr, func);
    memcpy(func->code, dst_ptr_global, func->code_length);

    DCFlushRange(func->code, func->code_length);
    ICInvalidateRange(func->code, func->code_length);

    insert_func(&ppc_block->funcs, func);

    return func;
}

static jump_node jumps[MAX_JUMPS];
static int jump_pos = 0;

int mips_is_jump(MIPS_instr instr){
    unsigned int op = MIPS_GET_OPCODE(instr);
    if(op == MIPS_OPCODE_J || op == MIPS_OPCODE_JAL)
        return 1;
    if(op == MIPS_OPCODE_R){
        unsigned int func = MIPS_GET_FUNC(instr);
        if(func == MIPS_FUNC_JR || func == MIPS_FUNC_JALR)
            return 1;
    }
    if(op == MIPS_OPCODE_BEQ || op == MIPS_OPCODE_BNE ||
       op == MIPS_OPCODE_BLEZ || op == MIPS_OPCODE_BGTZ ||
       op == MIPS_OPCODE_BEQL || op == MIPS_OPCODE_BNEL ||
       op == MIPS_OPCODE_BLEZL || op == MIPS_OPCODE_BGTZL)
        return 1;
    unsigned int rt = MIPS_GET_RA(instr);
    if(op == MIPS_OPCODE_B &&
       (rt == MIPS_RT_BLTZ || rt == MIPS_RT_BGEZ ||
        rt == MIPS_RT_BLTZL || rt == MIPS_RT_BGEZL ||
        rt == MIPS_RT_BLTZAL || rt == MIPS_RT_BGEZAL ||
        rt == MIPS_RT_BLTZALL || rt == MIPS_RT_BGEZALL))
        return 1;
    return 0;
}

int add_jump(int old_jump, int is_j, int is_call){
    if(jump_pos >= MAX_JUMPS) return -1;
    int pos = jump_pos++;
    jumps[pos].src_pc = src_pc_val;
    jumps[pos].dst_instr = dst_ptr_global;
    jumps[pos].old_jump = old_jump;
    jumps[pos].new_jump = 0;
    jumps[pos].type = 0;
    if(is_j)      jumps[pos].type |= JUMP_TYPE_J;
    if(is_call)   jumps[pos].type |= JUMP_TYPE_CALL;
    return pos;
}

int is_j_out(int branch, int is_aa){
    return 0;
}

int is_j_dst(void){
    return 0;
}

int add_jump_special(int is_j){
    return add_jump(0, is_j, 0);
}

void set_jump_special(int which, int new_jump){
    if(which < jump_pos && which >= 0){
        jumps[which].new_jump = new_jump;
    }
}

PowerPC_block* blocks_get(unsigned int idx){
    if(idx < 0x100000)
        return blocks[idx];
    return NULL;
}

void init_block(PowerPC_block* ppc_block){
    if(ppc_block) {
        memset(ppc_block->flags, 0, sizeof(ppc_block->flags));
        ppc_block->funcs = NULL;
    }
    jump_pos = 0;
    invalidateRegisters();
}

void deinit_block(PowerPC_block* ppc_block){
    if(ppc_block){
        if(ppc_block->funcs){
            PowerPC_func_node* node = ppc_block->funcs;
            remove_outgoing_links(&node, NULL);
        }
        ppc_block->funcs = NULL;
    }
}

void invalidate_block(PowerPC_block* ppc_block){
    invalid_code[ppc_block->start_address>>12] = 0;
    if(ppc_block->funcs){
        PowerPC_func_node* node = ppc_block->funcs;
        remove_outgoing_links(&node, NULL);
    }
    ppc_block->funcs = NULL;
    memset(ppc_block->flags, 0, sizeof(ppc_block->flags));
}

char txtbuffer[1024];

int do_disasm = 0;

int disassemble(unsigned int a, unsigned int op){
    (void)a; (void)op;
    return 0;
}
