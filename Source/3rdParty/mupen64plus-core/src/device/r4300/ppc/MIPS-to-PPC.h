#ifndef MIPS_TO_PPC_H
#define MIPS_TO_PPC_H

#include "MIPS.h"
#include "PowerPC.h"

#define CONVERT_ERROR   -1
#define CONVERT_SUCCESS  0
#define CONVERT_WARNING  1
#define INTERPRETED      2

extern MIPS_instr get_next_src(void);
extern MIPS_instr peek_next_src(void);
extern PowerPC_instr* get_curr_dst(void);
extern void unget_last_src(void);
extern void nop_ignored(void);
extern unsigned int get_src_pc(void);
void reset_code_addr(void);
extern int  is_j_out(int branch, int is_aa);
extern int  add_jump_special(int is_j);
extern void set_jump_special(int which, int new_jump);
void start_new_block(void);
void start_new_mapping(void);

int convert(void);

void * rewriteDynaMemVM(void* fault_addr);

#endif
