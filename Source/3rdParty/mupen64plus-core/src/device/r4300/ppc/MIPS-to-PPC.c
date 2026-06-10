#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <assert.h>

#include "Recompile.h"
#include "MIPS-to-PPC.h"
#include "Register-Cache.h"
#include "Wrappers.h"
#include "Recomp-Cache.h"
#include <math.h>

#include "ppc_dynarec_compat.h"

/* libgcc builtins used as jump targets from recompiled code */
extern long long __fixdfdi(double);
extern long long __fixsfdi(float);
extern double __floatdidf(long long);
extern float __floatdisf(long long);

static int FP_need_check;
static int delaySlotNext, isDelaySlot;

static void genCallInterp(MIPS_instr);
#define JUMPTO_REG  0
#define JUMPTO_OFF  1
#define JUMPTO_ADDR 2
#define JUMPTO_OFF_SIZE  13

static void genJumpTo(unsigned int loc, unsigned int type);
static void genUpdateCount(int checkCount);
static void genCheckFP(void);
void genCallDynaMem(memType type, int base, short immed);
static int genCallDynaMemVM(int rs_reg, int rt_reg, memType type, int immed);

/* 64-bit-safe call: loads full target address and uses bctrl.
 *
 * Constructs 64-bit target from 4x16-bit immediates (lis/rldicl/ori) and
 * combines them. Avoids bl/mflr/LR trickery, embedded data, and alignment
 * issues. Stores final address to canary[12/13] via r31 for debugging.
 */
static void emit_64bit_call(uintptr_t target) {
    uint64_t t = (uint64_t)target;
    uint16_t w0 =  t        & 0xFFFF;
    uint16_t w1 = (t >> 16) & 0xFFFF;
    uint16_t w2 = (t >> 32) & 0xFFFF;
    uint16_t w3 = (t >> 48) & 0xFFFF;
    PowerPC_instr tmp;

    EMIT_LIS(12, w1);
    EMIT_STW(12, 4 * 4, 31);   /* canary[4] = r12 after LIS */
    EMIT_RLDICL(12, 12, 0, 32);
    EMIT_STW(12, 5 * 4, 31);   /* canary[5] = r12 after RLDICL */
    EMIT_ORI(12, 12, w0);
    EMIT_STW(12, 15 * 4, 31);  /* canary[15] = r12 after low32 construction */

    EMIT_LIS(11, w3);
    EMIT_RLDICL(11, 11, 0, 32);
    EMIT_ORI(11, 11, w2);
    GEN_RLDICR(tmp, 11, 11, 32, 31, 0);  /* sldi r11, r11, 32 */
    set_next_dst(tmp);
    EMIT_STW(11, 14 * 4, 31);  /* canary[14] = r11 after sldi (high32 part) */

    EMIT_OR(12, 12, 11);
    EMIT_STW(12, 15 * 4, 31);  /* canary[15] = r12 after OR (overwrites previous) */
    EMIT_STW(12, 6 * 4, 31);   /* canary[6] = r12 after OR (duplicate, not overwritten) */
    EMIT_LI(0, 0xBB);
    EMIT_STW(0, 12 * 4, 31);  /* canary[12] = 0xBB (flag we reached OR) */

    EMIT_MTCTR(12);
    EMIT_STW(12, 13 * 4, 31);  /* canary[13] = r12 right before bctrl */
    EMIT_BCTRL(0);
}

#define CANT_COMPILE_DELAY() \
    ((get_src_pc()&0xFFF) == 0xFFC && \
     (get_src_pc() <  0x80000000 || \
      get_src_pc() >= 0xC0000000))

static inline unsigned short extractUpper16(void* address){
    uintptr_t addr = (uintptr_t)address;
    return (addr>>16) + ((addr>>15)&1);
}

static inline short extractLower16(void* address){
    uintptr_t addr = (uintptr_t)address;
    return addr&0x8000 ? (addr&0xffff)-0x10000 : addr&0xffff;
}

static inline int check_delaySlot(void){
    if(peek_next_src() == 0){
        get_next_src();
        return 0;
    } else {
        if(mips_is_jump(peek_next_src())) return CONVERT_WARNING;
        delaySlotNext = 1;
        convert();
        return 1;
    }
}

#define MIPS_REG_HI 32
#define MIPS_REG_LO 33

void start_new_block(void){
    invalidateRegisters();
    if(mips_is_jump(peek_next_src())) delaySlotNext = 2;
    else delaySlotNext = 0;
}
void start_new_mapping(void){
    flushRegisters();
    FP_need_check = 1;
    reset_code_addr();
}

static inline int signExtend(int value, int size){
    int signMask = 1 << (size-1);
    int negMask = 0xffffffff << (size-1);
    if(value & signMask) value |= negMask;
    return value;
}

unsigned int src_pc_val;
MIPS_instr* src_ptr_global;
PowerPC_instr* dst_ptr_global;
int set_next_dst_override_val;
PowerPC_instr set_next_dst_instr_val;

MIPS_instr get_next_src(void){
    src_pc_val += 4;
    return *src_ptr_global++;
}

MIPS_instr peek_next_src(void){
    return *src_ptr_global;
}

PowerPC_instr* get_curr_dst(void){
    return dst_ptr_global;
}

void unget_last_src(void){
    src_ptr_global--;
    src_pc_val -= 4;
}

unsigned int get_src_pc(void){
    return src_pc_val - 4;
}

void set_next_dst(PowerPC_instr instr){
    if(set_next_dst_override_val){
        set_next_dst_instr_val = instr;
        set_next_dst_override_val = 0;
    } else {
        *dst_ptr_global++ = instr;
    }
}

/* reset_code_addr is defined in Recompile.c to track code_addr_buffer */

static int (*gen_ops[64])(MIPS_instr);

int convert(void){
    int needFlush = delaySlotNext;
    isDelaySlot = (delaySlotNext == 1);
    delaySlotNext = 0;

    MIPS_instr mips = get_next_src();
    int result = gen_ops[MIPS_GET_OPCODE(mips)](mips);

    if(needFlush) flushRegisters();
    return result;
}

static int NI(MIPS_instr mips){
    (void)mips;
    return CONVERT_ERROR;
}
static int NI2(MIPS_instr mips, int dbl){
    (void)mips; (void)dbl;
    return CONVERT_ERROR;
}

typedef enum { NONE=0, EQ, NE, LT, GT, LE, GE } condition;

static int branch(int offset, condition cond, int link, int likely){
    int likely_id;
    int bo, bi, nbo;
    switch(cond){
        case EQ:  bo = 0x12; nbo = 0x10; bi = 18; break;
        case NE:  bo = 0x10; nbo = 0x12; bi = 18; break;
        case LT:  bo = 0x12; nbo = 0x10; bi = 16; break;
        case GE:  bo = 0x10; nbo = 0x12; bi = 16; break;
        case GT:  bo = 0x12; nbo = 0x10; bi = 17; break;
        case LE:  bo = 0x10; nbo = 0x12; bi = 17; break;
        default:  bo = 0x14; nbo = 0x14; bi = 19; break;
    }

    flushRegisters();

    if(link){
        int lr = mapRegisterNew(MIPS_REG_LR);
        EMIT_LIS(lr, (get_src_pc()+8)>>16);
        EMIT_ORI(lr, lr, get_src_pc()+8);
        flushRegisters();
    }

    if(likely){
        likely_id = add_jump_special(0);
        EMIT_BC(likely_id, 0, 0, nbo, bi);
    }

    PowerPC_instr* preDelay = get_curr_dst();
    check_delaySlot();
    int delaySlot = get_curr_dst() - preDelay;

    if(likely) set_jump_special(likely_id, delaySlot+1);

    genUpdateCount(1);

    if(is_j_out(offset, 0)){
        EMIT_BC(JUMPTO_OFF_SIZE+1, 0, 0, nbo, bi);
        genJumpTo(offset, JUMPTO_OFF);
        EMIT_LIS(3, (get_src_pc()+4)>>16);
        EMIT_ORI(3, 3, get_src_pc()+4);
        EMIT_BLELR(2, 0);
    } else {
        if(cond != NONE){
            EMIT_BC(4, 0, 0, bo, bi);
            EMIT_LIS(3, (get_src_pc() + 4)>>16);
            EMIT_ORI(3, 3, get_src_pc() + 4);
            EMIT_B(3, 0, 0);
        }
        EMIT_LIS(3, (get_src_pc() + (offset<<2))>>16);
        EMIT_ORI(3, 3, get_src_pc() + (offset<<2));
        EMIT_STW(3, 0, DYNAREG_LADDR);
        EMIT_BLELR(2, 0);
        EMIT_BC(2, 0, 0, nbo, bi);
        if(offset < 0){
            EMIT_LD(0, DYNAOFF_LR, 1);
            EMIT_MTLR(0);
            EMIT_BLR(0);
        } else {
            EMIT_B(add_jump(offset, 0, 0), 0, 0);
        }
    }

    if(delaySlot){
        if(is_j_dst() && !is_j_out(0, 0)){
            EMIT_B(delaySlot+1, 0, 0);
            unget_last_src();
            delaySlotNext = 2;
        }
    } else nop_ignored();

    return CONVERT_SUCCESS;
}

static int J(MIPS_instr mips){
    unsigned int naddr = (MIPS_GET_LI(mips)<<2)|((get_src_pc()+4)&0xf0000000);
    if(naddr == get_src_pc() || CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    flushRegisters();
    reset_code_addr();
    PowerPC_instr* preDelay = get_curr_dst();
    check_delaySlot();
    int delaySlot = get_curr_dst() - preDelay;
    genUpdateCount(1);
    if(is_j_out(MIPS_GET_LI(mips), 1)){
        genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
    } else {
        EMIT_LIS(3, naddr>>16);
        EMIT_ORI(3, 3, naddr);
        EMIT_STW(3, 0, DYNAREG_LADDR);
        EMIT_BLELR(2, 0);
        EMIT_B(add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
    }
    if(delaySlot){ if(is_j_dst()){ unget_last_src(); delaySlotNext = 2; } }
    else nop_ignored();
    return CONVERT_SUCCESS;
}

static int JAL(MIPS_instr mips){
    unsigned int naddr = (MIPS_GET_LI(mips)<<2)|((get_src_pc()+4)&0xf0000000);
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    flushRegisters();
    reset_code_addr();
    PowerPC_instr* preDelay = get_curr_dst();
    check_delaySlot();
    int delaySlot = get_curr_dst() - preDelay;
    genUpdateCount(1);
    int lr = mapRegisterNew(MIPS_REG_LR);
    EMIT_LIS(lr, (get_src_pc()+4)>>16);
    EMIT_ORI(lr, lr, get_src_pc()+4);
    flushRegisters();
    if(is_j_out(MIPS_GET_LI(mips), 1)){
        genJumpTo(MIPS_GET_LI(mips), JUMPTO_ADDR);
    } else {
        EMIT_LIS(3, naddr>>16);
        EMIT_ORI(3, 3, naddr);
        EMIT_STW(3, 0, DYNAREG_LADDR);
        EMIT_BLELR(2, 0);
        EMIT_B(add_jump(MIPS_GET_LI(mips), 1, 0), 0, 0);
    }
    if(delaySlot){ if(is_j_dst()){ unget_last_src(); delaySlotNext = 2; } }
    else nop_ignored();
    return CONVERT_SUCCESS;
}

static void genCmp64(int cr, int _ra, int _rb){
    if(getRegisterMapping(_ra) == MAPPING_32 ||
       getRegisterMapping(_rb) == MAPPING_32){
        int ra = mapRegister(_ra), rb = mapRegister(_rb);
        EMIT_CMP(ra, rb, 4);
    } else {
        RegMapping ra = mapRegister64(_ra), rb = mapRegister64(_rb);
        EMIT_CMP(ra.hi, rb.hi, 4);
        EMIT_BNE(4, 2, 0, 0);
        EMIT_CMPL(ra.lo, rb.lo, 4);
    }
}

static void genCmpi64(int cr, int _ra, short immed){
    if(getRegisterMapping(_ra) == MAPPING_32){
        int ra = mapRegister(_ra);
        EMIT_CMPI(ra, immed, 4);
    } else {
        RegMapping ra = mapRegister64(_ra);
        EMIT_CMPI(ra.hi, (immed&0x8000) ? ~0 : 0, 4);
        EMIT_BNE(4, 2, 0, 0);
        EMIT_CMPLI(ra.lo, immed, 4);
    }
}

static int BEQ(MIPS_instr mips){
    if((MIPS_GET_IMMED(mips) == 0xffff &&
        MIPS_GET_RA(mips) == MIPS_GET_RB(mips)) ||
       CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmp64(4, MIPS_GET_RA(mips), MIPS_GET_RB(mips));
    return branch(signExtend(MIPS_GET_IMMED(mips),16), EQ, 0, 0);
}

static int BNE(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmp64(4, MIPS_GET_RA(mips), MIPS_GET_RB(mips));
    return branch(signExtend(MIPS_GET_IMMED(mips),16), NE, 0, 0);
}

static int BLEZ(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmpi64(4, MIPS_GET_RA(mips), 0);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), LE, 0, 0);
}

static int BGTZ(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmpi64(4, MIPS_GET_RA(mips), 0);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), GT, 0, 0);
}

static int ADDIU(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    EMIT_ADDI(mapRegisterNew( MIPS_GET_RT(mips) ), rs, MIPS_GET_IMMED(mips));
    return CONVERT_SUCCESS;
}

static int ADDI(MIPS_instr mips) { return ADDIU(mips); }

static int SLTI(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegisterNew( MIPS_GET_RT(mips) );
    int tmp = (rs == rt) ? mapRegisterTemp() : rt;
    EMIT_ADDI(tmp, 0, MIPS_GET_IMMED(mips));
    EMIT_SUBFC(0, tmp, rs);
    EMIT_EQV(rt, tmp, rs);
    EMIT_SRWI(rt, rt, 31);
    EMIT_ADDZE(rt, rt);
    EMIT_RLWINM(rt, rt, 0, 31, 31);
    if(rs == rt) unmapRegisterTemp(tmp);
    return CONVERT_SUCCESS;
}

static int SLTIU(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegisterNew( MIPS_GET_RT(mips) );
    EMIT_ADDI(0, 0, MIPS_GET_IMMED(mips));
    EMIT_SUBFC(rt, 0, rs);
    EMIT_SUBFE(rt, rt, rt);
    EMIT_NEG(rt, rt);
    return CONVERT_SUCCESS;
}

static int ANDI(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegisterNew( MIPS_GET_RT(mips) );
    EMIT_ANDI(rt, rs, MIPS_GET_IMMED(mips));
    return CONVERT_SUCCESS;
}

static int ORI(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rt = mapRegister64New( MIPS_GET_RT(mips) );
    EMIT_OR(rt.hi, rs.hi, rs.hi);
    EMIT_ORI(rt.lo, rs.lo, MIPS_GET_IMMED(mips));
    return CONVERT_SUCCESS;
}

static int XORI(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rt = mapRegister64New( MIPS_GET_RT(mips) );
    EMIT_OR(rt.hi, rs.hi, rs.hi);
    EMIT_XORI(rt.lo, rs.lo, MIPS_GET_IMMED(mips));
    return CONVERT_SUCCESS;
}

static int LUI(MIPS_instr mips){
    EMIT_LIS(mapRegisterNew( MIPS_GET_RT(mips) ), MIPS_GET_IMMED(mips));
    return CONVERT_SUCCESS;
}

static int BEQL(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmp64(4, MIPS_GET_RA(mips), MIPS_GET_RB(mips));
    return branch(signExtend(MIPS_GET_IMMED(mips),16), EQ, 0, 1);
}

static int BNEL(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmp64(4, MIPS_GET_RA(mips), MIPS_GET_RB(mips));
    return branch(signExtend(MIPS_GET_IMMED(mips),16), NE, 0, 1);
}

static int BLEZL(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmpi64(4, MIPS_GET_RA(mips), 0);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), LE, 0, 1);
}

static int BGTZL(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCmpi64(4, MIPS_GET_RA(mips), 0);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), GT, 0, 1);
}

static int DADDIU(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rt = mapRegister64New( MIPS_GET_RT(mips) );
    EMIT_ADDI(0, 0, (MIPS_GET_IMMED(mips)&0x8000) ? ~0 : 0);
    EMIT_ADDIC(rt.lo, rs.lo, MIPS_GET_IMMED(mips));
    EMIT_ADDE(rt.hi, rs.hi, 0);
    return CONVERT_SUCCESS;
}

static int DADDI(MIPS_instr mips) { return DADDIU(mips); }

static int LDL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int LDR(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int LB(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LB,MIPS_GET_IMMED(mips));
}

static int LH(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LH,MIPS_GET_IMMED(mips));
}

static int LWL(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LWL,MIPS_GET_IMMED(mips));
}

static int LW(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LW,MIPS_GET_IMMED(mips));
}

static int LBU(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LBU,MIPS_GET_IMMED(mips));
}

static int LHU(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LHU,MIPS_GET_IMMED(mips));
}

static int LWR(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LWR,MIPS_GET_IMMED(mips));
}

static int LWU(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LWU,MIPS_GET_IMMED(mips));
}

static int LD(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LD,MIPS_GET_IMMED(mips));
}

static int LWC1(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LWC1,MIPS_GET_IMMED(mips));
}

static int LDC1(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_LDC1,MIPS_GET_IMMED(mips));
}

static int SB(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SB,MIPS_GET_IMMED(mips));
}

static int SH(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SH,MIPS_GET_IMMED(mips));
}

static int SWL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SW(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SW,MIPS_GET_IMMED(mips));
}

static int SDL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SDR(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SWR(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SD(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SD,MIPS_GET_IMMED(mips));
}

static int SWC1(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SWC1,MIPS_GET_IMMED(mips));
}

static int SDC1(MIPS_instr mips){
    return genCallDynaMemVM(MIPS_GET_RS(mips),MIPS_GET_RT(mips),MEM_SDC1,MIPS_GET_IMMED(mips));
}

static int CACHE(MIPS_instr mips){ return CONVERT_ERROR; }

static int LL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SC(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SLL(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_SLWI(rd, rt, MIPS_GET_SA(mips));
    return CONVERT_SUCCESS;
}

static int SRL(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_SRWI(rd, rt, MIPS_GET_SA(mips));
    return CONVERT_SUCCESS;
}

static int SRA(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_SRAWI(rd, rt, MIPS_GET_SA(mips));
    return CONVERT_SUCCESS;
}

static int SLLV(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_RLWINM(0, rs, 0, 27, 31);
    EMIT_SLW(rd, rt, 0);
    return CONVERT_SUCCESS;
}

static int SRLV(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_RLWINM(0, rs, 0, 27, 31);
    EMIT_SRW(rd, rt, 0);
    return CONVERT_SUCCESS;
}

static int SRAV(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_RLWINM(0, rs, 0, 27, 31);
    EMIT_SRAW(rd, rt, 0);
    return CONVERT_SUCCESS;
}

static int JR(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    flushRegisters();
    reset_code_addr();
    EMIT_STW(mapRegister(MIPS_GET_RS(mips)), REG_LOCALRS*8+4, DYNAREG_REG);
    invalidateRegisters();
    PowerPC_instr* preDelay = get_curr_dst();
    check_delaySlot();
    int delaySlot = get_curr_dst() - preDelay;
    genUpdateCount(0);
    genJumpTo(REG_LOCALRS, JUMPTO_REG);
    if(delaySlot){ if(is_j_dst()){ unget_last_src(); delaySlotNext = 2; } }
    else nop_ignored();
    return INTERPRETED;
}

static int JALR(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    flushRegisters();
    reset_code_addr();
    EMIT_STW(mapRegister(MIPS_GET_RS(mips)), REG_LOCALRS*8+4, DYNAREG_REG);
    invalidateRegisters();
    PowerPC_instr* preDelay = get_curr_dst();
    check_delaySlot();
    int delaySlot = get_curr_dst() - preDelay;
    genUpdateCount(0);
    int rd = mapRegisterNew(MIPS_GET_RD(mips));
    EMIT_LIS(rd, (get_src_pc()+4)>>16);
    EMIT_ORI(rd, rd, get_src_pc()+4);
    flushRegisters();
    genJumpTo(REG_LOCALRS, JUMPTO_REG);
    if(delaySlot){ if(is_j_dst()){ unget_last_src(); delaySlotNext = 2; } }
    else nop_ignored();
    return INTERPRETED;
}

static int SYSCALL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int BREAK(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int SYNC(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MFHI(MIPS_instr mips){
    RegMapping hi = mapRegister64( MIPS_REG_HI );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_OR(rd.lo, hi.lo, hi.lo);
    EMIT_OR(rd.hi, hi.hi, hi.hi);
    return CONVERT_SUCCESS;
}

static int MTHI(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping hi = mapRegister64New( MIPS_REG_HI );
    EMIT_OR(hi.lo, rs.lo, rs.lo);
    EMIT_OR(hi.hi, rs.hi, rs.hi);
    return CONVERT_SUCCESS;
}

static int MFLO(MIPS_instr mips){
    RegMapping lo = mapRegister64( MIPS_REG_LO );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_OR(rd.lo, lo.lo, lo.lo);
    EMIT_OR(rd.hi, lo.hi, lo.hi);
    return CONVERT_SUCCESS;
}

static int MTLO(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping lo = mapRegister64New( MIPS_REG_LO );
    EMIT_OR(lo.lo, rs.lo, rs.lo);
    EMIT_OR(lo.hi, rs.hi, rs.hi);
    return CONVERT_SUCCESS;
}

static int MULT(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int hi = mapRegisterNew( MIPS_REG_HI );
    int lo = mapRegisterNew( MIPS_REG_LO );
    if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
        EMIT_MULLW(lo, rs, rt);
        EMIT_MULHW(hi, rs, rt);
    } else {
        EMIT_LI(lo, 0);
        EMIT_LI(hi, 0);
    }
    return CONVERT_SUCCESS;
}

static int MULTU(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int hi = mapRegisterNew( MIPS_REG_HI );
    int lo = mapRegisterNew( MIPS_REG_LO );
    if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
        EMIT_MULLW(lo, rs, rt);
        EMIT_MULHWU(hi, rs, rt);
    } else {
        EMIT_LI(lo, 0);
        EMIT_LI(hi, 0);
    }
    return CONVERT_SUCCESS;
}

static int DIV(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int hi = mapRegisterNew( MIPS_REG_HI );
    int lo = mapRegisterNew( MIPS_REG_LO );
    if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
        EMIT_DIVW(lo, rs, rt);
        EMIT_MULLW(hi, lo, rt);
        EMIT_SUBF(hi, hi, rs);
    }
    return CONVERT_SUCCESS;
}

static int DIVU(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int hi = mapRegisterNew( MIPS_REG_HI );
    int lo = mapRegisterNew( MIPS_REG_LO );
    if(MIPS_GET_RS(mips) && MIPS_GET_RT(mips)){
        EMIT_DIVWU(lo, rs, rt);
        EMIT_MULLW(hi, lo, rt);
        EMIT_SUBF(hi, hi, rs);
    }
    return CONVERT_SUCCESS;
}

static int DSLLV(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int sa = mapRegisterTemp();
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_RLWINM(sa, rs, 0, 26, 31);
    EMIT_SLW(rd.hi, rt.hi, sa);
    EMIT_SUBFIC(0, sa, 32);
    EMIT_SRW(0, rt.lo, 0);
    EMIT_OR(rd.hi, rd.hi, 0);
    EMIT_ADDI(0, sa, -32);
    EMIT_SLW(0, rt.lo, 0);
    EMIT_OR(rd.hi, rd.hi, 0);
    EMIT_SLW(rd.lo, rt.lo, sa);
    unmapRegisterTemp(sa);
    return CONVERT_SUCCESS;
}

static int DSRLV(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int sa = mapRegisterTemp();
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_RLWINM(sa, rs, 0, 26, 31);
    EMIT_SRW(rd.lo, rt.lo, sa);
    EMIT_SUBFIC(0, sa, 32);
    EMIT_SLW(0, rt.hi, 0);
    EMIT_OR(rd.lo, rd.lo, 0);
    EMIT_ADDI(0, sa, -32);
    EMIT_SRW(0, rt.hi, 0);
    EMIT_OR(rd.lo, rd.lo, 0);
    EMIT_SRW(rd.hi, rt.hi, sa);
    unmapRegisterTemp(sa);
    return CONVERT_SUCCESS;
}

static int DSRAV(MIPS_instr mips){
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int sa = mapRegisterTemp();
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_RLWINM(sa, rs, 0, 26, 31);
    EMIT_CMPI(sa, 32, 1);
    EMIT_SRW(rd.lo, rt.lo, sa);
    EMIT_BGE(1, 5, 0, 0);
    EMIT_SUBFIC(0, sa, 32);
    EMIT_SLW(0, rt.hi, 0);
    EMIT_OR(rd.lo, rd.lo, 0);
    EMIT_B(4, 0, 0);
    EMIT_ADDI(0, sa, -32);
    EMIT_SRAW(0, rt.hi, 0);
    EMIT_OR(rd.lo, rd.lo, 0);
    EMIT_SRAW(rd.hi, rt.hi, sa);
    unmapRegisterTemp(sa);
    return CONVERT_SUCCESS;
}

static int DMULT(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int DMULTU(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int DDIV(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int DDIVU(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int DADDU(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_ADDC(rd.lo, rs.lo, rt.lo);
    EMIT_ADDE(rd.hi, rs.hi, rt.hi);
    return CONVERT_SUCCESS;
}

static int DADD(MIPS_instr mips) { return DADDU(mips); }

static int DSUBU(MIPS_instr mips){
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_SUBFC(rd.lo, rt.lo, rs.lo);
    EMIT_SUBFE(rd.hi, rt.hi, rs.hi);
    return CONVERT_SUCCESS;
}

static int DSUB(MIPS_instr mips) { return DSUBU(mips); }

static int DSLL(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    if(sa){
        EMIT_SLWI(rd.hi, rt.hi, sa);
        EMIT_RLWINM(0, rt.lo, sa, 32-sa, 31);
        EMIT_OR(rd.hi, rd.hi, 0);
        EMIT_SLWI(rd.lo, rt.lo, sa);
    } else {
        EMIT_ADDI(rd.hi, rt.hi, 0);
        EMIT_ADDI(rd.lo, rt.lo, 0);
    }
    return CONVERT_SUCCESS;
}

static int DSRL(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    if(sa){
        EMIT_SRWI(rd.lo, rt.lo, sa);
        EMIT_RLWINM(0, rt.hi, 32-sa, 0, sa-1);
        EMIT_OR(rd.lo, rd.lo, 0);
        EMIT_SRWI(rd.hi, rt.hi, sa);
    } else {
        EMIT_ADDI(rd.hi, rt.hi, 0);
        EMIT_ADDI(rd.lo, rt.lo, 0);
    }
    return CONVERT_SUCCESS;
}

static int DSRA(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    if(sa){
        EMIT_SRWI(rd.lo, rt.lo, sa);
        EMIT_RLWINM(0, rt.hi, 32-sa, 0, sa-1);
        EMIT_OR(rd.lo, rd.lo, 0);
        EMIT_SRAWI(rd.hi, rt.hi, sa);
    } else {
        EMIT_ADDI(rd.hi, rt.hi, 0);
        EMIT_ADDI(rd.lo, rt.lo, 0);
    }
    return CONVERT_SUCCESS;
}

static int DSLL32(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    EMIT_SLWI(rd.hi, rt.lo, sa);
    EMIT_ADDI(rd.lo, 0, 0);
    return CONVERT_SUCCESS;
}

static int DSRL32(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    EMIT_SRWI(rd.lo, rt.hi, sa);
    EMIT_ADDI(rd.hi, 0, 0);
    return CONVERT_SUCCESS;
}

static int DSRA32(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    int sa = MIPS_GET_SA(mips);
    EMIT_SRAWI(rd.lo, rt.hi, sa);
    EMIT_SRAWI(rd.hi, rt.hi, 31);
    return CONVERT_SUCCESS;
}

static int ADDU(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    EMIT_ADD(mapRegisterNew( MIPS_GET_RD(mips) ), rs, rt);
    return CONVERT_SUCCESS;
}

static int ADD(MIPS_instr mips) { return ADDU(mips); }

static int SUBU(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    EMIT_SUB(mapRegisterNew( MIPS_GET_RD(mips) ), rs, rt);
    return CONVERT_SUCCESS;
}

static int SUB(MIPS_instr mips) { return SUBU(mips); }

static int AND(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_AND(rd.hi, rs.hi, rt.hi);
    EMIT_AND(rd.lo, rs.lo, rt.lo);
    return CONVERT_SUCCESS;
}

static int OR(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_OR(rd.hi, rs.hi, rt.hi);
    EMIT_OR(rd.lo, rs.lo, rt.lo);
    return CONVERT_SUCCESS;
}

static int XOR(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_XOR(rd.hi, rs.hi, rt.hi);
    EMIT_XOR(rd.lo, rs.lo, rt.lo);
    return CONVERT_SUCCESS;
}

static int NOR(MIPS_instr mips){
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    RegMapping rs = mapRegister64( MIPS_GET_RS(mips) );
    RegMapping rd = mapRegister64New( MIPS_GET_RD(mips) );
    EMIT_NOR(rd.hi, rs.hi, rt.hi);
    EMIT_NOR(rd.lo, rs.lo, rt.lo);
    return CONVERT_SUCCESS;
}

static int SLT(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_SUBFC(0, rt, rs);
    EMIT_EQV(rd, rt, rs);
    EMIT_SRWI(rd, rd, 31);
    EMIT_ADDZE(rd, rd);
    EMIT_RLWINM(rd, rd, 0, 31, 31);
    return CONVERT_SUCCESS;
}

static int SLTU(MIPS_instr mips){
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int rs = mapRegister( MIPS_GET_RS(mips) );
    int rd = mapRegisterNew( MIPS_GET_RD(mips) );
    EMIT_SUBFC(rd, rt, rs);
    EMIT_SUBFE(rd, rd, rd);
    EMIT_NEG(rd, rd);
    return CONVERT_SUCCESS;
}

static int TEQ(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MADD(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MADDU(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MUL(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MSUB(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int MSUBU(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int CLZ(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int CLO(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int (*gen_special[64])(MIPS_instr) = {
   SLL , NI   , SRL , SRA , SLLV   , NI    , SRLV  , SRAV  ,
   JR  , JALR , MADD, MADDU, SYSCALL, BREAK , MUL   , SYNC  ,
   MFHI, MTHI , MFLO, MTLO, DSLLV  , NI    , DSRLV , DSRAV ,
   MULT, MULTU, DIV , DIVU, DMULT  , DMULTU, DDIV  , DDIVU ,
   ADD , ADDU , SUB , SUBU, AND    , OR    , XOR   , NOR   ,
   NI  , NI   , SLT , SLTU, DADD   , DADDU , DSUB  , DSUBU ,
   MSUB, MSUBU, CLZ , CLO , TEQ    , NI    , NI    , NI    ,
   DSLL, NI   , DSRL, DSRA, DSLL32 , NI    , DSRL32, DSRA32
};

static int SPECIAL(MIPS_instr mips){
    return gen_special[MIPS_GET_FUNC(mips)](mips);
}

static int REGIMM(MIPS_instr mips){
    int which = MIPS_GET_RT(mips);
    int cond   = which & 1;
    int likely = which & 2;
    int link   = which & 16;

    if(MIPS_GET_IMMED(mips) == 0xffff || CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }

    genCmpi64(4, MIPS_GET_RA(mips), 0);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), cond ? GE : LT, link, likely);
}

static int TLBR(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int TLBWI(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int TLBWR(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int TLBP(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int ERET(MIPS_instr mips){
    flushRegisters();
    genUpdateCount(0);
    EMIT_LWZ(3, 12*4, DYNAREG_COP0);
    EMIT_LIS(4, extractUpper16(&llbit));
    EMIT_RLWINM(3, 3, 0, 31, 29);
    EMIT_STW(DYNAREG_ZERO, extractLower16(&llbit), 4);
    EMIT_STW(3, 12*4, DYNAREG_COP0);
    emit_64bit_call((uintptr_t)(&check_interupt));
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_LWZ(3, 14*4, DYNAREG_COP0);
    EMIT_MTLR(0);
    EMIT_BLR(0);
    return CONVERT_SUCCESS;
}

static int DERET(MIPS_instr mips){
    genCallInterp(mips);
    return INTERPRETED;
}

static int (*gen_tlb[64])(MIPS_instr) = {
   NI  , TLBR, TLBWI, NI, NI, NI, TLBWR, NI,
   TLBP, NI  , NI   , NI, NI, NI, NI   , NI,
   NI  , NI  , NI   , NI, NI, NI, NI   , NI,
   ERET, NI  , NI   , NI, NI, NI, NI   , NI,
   NI  , NI  , NI   , NI, NI, NI, NI   , NI,
   NI  , NI  , NI   , NI, NI, NI, NI   , NI,
   NI  , NI  , NI   , NI, NI, NI, NI   , NI,
   NI  , NI  , NI   , NI, NI, NI, NI   , NI
};

static int MFC0(MIPS_instr mips){
    int rt = mapRegisterNew(MIPS_GET_RT(mips));
    EMIT_LWZ(rt, MIPS_GET_RD(mips)*4, DYNAREG_COP0);
    return CONVERT_SUCCESS;
}

static int MTC0(MIPS_instr mips){
    int rt = MIPS_GET_RT(mips), rrt;
    int rd = MIPS_GET_RD(mips);
    int tmp;
    switch(rd){
    case 0:
        rrt = mapRegister(rt);
        EMIT_RLWINM(0, rrt, 0, 26, 0);
        EMIT_STW(0, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 2:
    case 3:
        rrt = mapRegister(rt);
        EMIT_RLWINM(0, rrt, 0, 2, 31);
        EMIT_STW(0, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 4:
        rrt = mapRegister(rt); tmp = mapRegisterTemp();
        EMIT_LWZ(tmp, rd*4, DYNAREG_COP0);
        EMIT_RLWINM(0, rrt, 0, 0, 8);
        EMIT_RLWINM(tmp, tmp, 0, 9, 27);
        EMIT_OR(tmp, tmp, 0);
        EMIT_STW(tmp, rd*4, DYNAREG_COP0);
        unmapRegisterTemp(tmp);
        return CONVERT_SUCCESS;
    case 5:
        rrt = mapRegister(rt);
        EMIT_RLWINM(0, rrt, 0, 7, 18);
        EMIT_STW(0, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 6:
        rrt = mapRegister(rt);
        EMIT_ADDI(0, 0, 31);
        EMIT_STW(rrt, rd*4, DYNAREG_COP0);
        EMIT_STW(0, 1*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 10:
        rrt = mapRegister(rt);
        EMIT_RLWINM(0, rrt, 0, 24, 18);
        EMIT_STW(0, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 13:
        rrt = mapRegister(rt);
        EMIT_STW(rrt, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 14:
    case 16:
    case 18:
    case 19:
        rrt = mapRegister(rt);
        EMIT_STW(rrt, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 28:
        rrt = mapRegister(rt);
        EMIT_RLWINM(0, rrt, 0, 4, 25);
        EMIT_STW(0, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 29:
        EMIT_STW(DYNAREG_ZERO, rd*4, DYNAREG_COP0);
        return CONVERT_SUCCESS;
    case 1:
    case 8:
    case 15:
    case 27:
        return CONVERT_SUCCESS;
    case 9:
    case 11:
    case 12:
    default:
        genCallInterp(mips);
        return INTERPRETED;
    }
}

static int TLB(MIPS_instr mips){
    return gen_tlb[mips&0x3f](mips);
}

static int (*gen_cop0[32])(MIPS_instr) = {
   MFC0, NI, NI, NI, MTC0, NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI,
   TLB , NI, NI, NI, NI  , NI, NI, NI,
   NI  , NI, NI, NI, NI  , NI, NI, NI
};

static int COP0(MIPS_instr mips){
    return gen_cop0[MIPS_GET_RS(mips)](mips);
}

static int MFC1(MIPS_instr mips){
    genCheckFP();
    int fs = MIPS_GET_FS(mips);
    int rt = mapRegisterNew( MIPS_GET_RT(mips) );
    flushFPR(fs);
    EMIT_LWZ(rt, fs*4, DYNAREG_FPR_32);
    EMIT_LWZ(rt, 0, rt);
    return CONVERT_SUCCESS;
}

static int DMFC1(MIPS_instr mips){
    genCheckFP();
    int fs = MIPS_GET_FS(mips);
    RegMapping rt = mapRegister64New( MIPS_GET_RT(mips) );
    int addr = mapRegisterTemp();
    flushFPR(fs);
    EMIT_LWZ(addr, fs*4, DYNAREG_FPR_64);
    EMIT_LWZ(rt.hi, 0, addr);
    EMIT_LWZ(rt.lo, 4, addr);
    unmapRegisterTemp(addr);
    return CONVERT_SUCCESS;
}

static int CFC1(MIPS_instr mips){
    genCheckFP();
    if(MIPS_GET_FS(mips) == 31){
        int rt = mapRegisterNew( MIPS_GET_RT(mips) );
        EMIT_LWZ(rt, 0, DYNAREG_FCR31);
    } else if(MIPS_GET_FS(mips) == 0){
        int rt = mapRegisterNew( MIPS_GET_RT(mips) );
        EMIT_LI(rt, 0x511);
    }
    return CONVERT_SUCCESS;
}

static int MTC1(MIPS_instr mips){
    genCheckFP();
    int rt = mapRegister( MIPS_GET_RT(mips) );
    int fs = MIPS_GET_FS(mips);
    int addr = mapRegisterTemp();
    invalidateFPR(fs);
    EMIT_LWZ(addr, fs*4, DYNAREG_FPR_32);
    EMIT_STW(rt, 0, addr);
    unmapRegisterTemp(addr);
    return CONVERT_SUCCESS;
}

static int DMTC1(MIPS_instr mips){
    genCheckFP();
    RegMapping rt = mapRegister64( MIPS_GET_RT(mips) );
    int fs = MIPS_GET_FS(mips);
    int addr = mapRegisterTemp();
    invalidateFPR(fs);
    EMIT_LWZ(addr, fs*4, DYNAREG_FPR_64);
    EMIT_STW(rt.hi, 0, addr);
    EMIT_STW(rt.lo, 4, addr);
    unmapRegisterTemp(addr);
    return CONVERT_SUCCESS;
}

static int CTC1(MIPS_instr mips){
    genCheckFP();
    if(MIPS_GET_FS(mips) == 31){
        int rt = mapRegister( MIPS_GET_RT(mips) );
        EMIT_STW(rt, 0, DYNAREG_FCR31);
    }
    return CONVERT_SUCCESS;
}

static int BC(MIPS_instr mips){
    if(CANT_COMPILE_DELAY()){
        genCallInterp(mips);
        return INTERPRETED;
    }
    genCheckFP();
    int cond   = mips & 0x00010000;
    int likely = mips & 0x00020000;
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_RLWINM(0, 0, 9, 31, 31);
    EMIT_CMPI(0, 0, 4);
    return branch(signExtend(MIPS_GET_IMMED(mips),16), cond?NE:EQ, 0, likely);
}

static int ADD_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FADD(fd, fs, ft, dbl);
    return CONVERT_SUCCESS;
}

static int SUB_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FSUB(fd, fs, ft, dbl);
    return CONVERT_SUCCESS;
}

static int MUL_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FMUL(fd, fs, ft, dbl);
    return CONVERT_SUCCESS;
}

static int DIV_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FDIV(fd, fs, ft, dbl);
    return CONVERT_SUCCESS;
}

static int SQRT_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fr=mapFPR( MIPS_GET_FS(mips), dbl );
    EMIT_FMR(1,fr);
    emit_64bit_call((dbl ? (uintptr_t)&sqrt : (uintptr_t)&sqrtf));
    fr=mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FMR(fr,1);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_MTLR(0);
    return CONVERT_SUCCESS;
}

static int ABS_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FABS(fd, fs);
    return CONVERT_SUCCESS;
}

static int MOV_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FMR(fd, fs);
    return CONVERT_SUCCESS;
}

static int NEG_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    EMIT_FNEG(fd, fs);
    return CONVERT_SUCCESS;
}

#define PPC_ROUNDING_NEAREST 0
#define PPC_ROUNDING_TRUNC   1
#define PPC_ROUNDING_CEIL    2
#define PPC_ROUNDING_FLOOR   3

static void set_rounding(int rounding_mode){
    EMIT_MTFSFI(7, rounding_mode);
}

static void set_rounding_reg(int fs){
    EMIT_MTFSF(1, fs);
}

static int ROUND_L_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR( MIPS_GET_FS(mips) );
    EMIT_FMR(1,fs);
    emit_64bit_call((dbl ? (uintptr_t)&round : (uintptr_t)&roundf));
    emit_64bit_call((dbl ? (uintptr_t)&__fixdfdi : (uintptr_t)&__fixsfdi));
    int addr = 5;
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_64);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_STW(3, 0, addr);
    EMIT_MTLR(0);
    EMIT_STW(4, 4, addr);
    return CONVERT_SUCCESS;
}

static int TRUNC_L_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR( MIPS_GET_FS(mips) );
    EMIT_FMR(1,fs);
    emit_64bit_call((dbl ? (uintptr_t)&__fixdfdi : (uintptr_t)&__fixsfdi));
    int addr = 5;
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_64);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_STW(3, 0, addr);
    EMIT_MTLR(0);
    EMIT_STW(4, 4, addr);
    return CONVERT_SUCCESS;
}

static int CEIL_L_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR( MIPS_GET_FS(mips) );
    EMIT_FMR(1,fs);
    emit_64bit_call((dbl ? (uintptr_t)&ceil : (uintptr_t)&ceilf));
    emit_64bit_call((dbl ? (uintptr_t)&__fixdfdi : (uintptr_t)&__fixsfdi));
    int addr = 5;
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_64);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_STW(3, 0, addr);
    EMIT_MTLR(0);
    EMIT_STW(4, 4, addr);
    return CONVERT_SUCCESS;
}

static int FLOOR_L_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR( MIPS_GET_FS(mips) );
    EMIT_FMR(1,fs);
    emit_64bit_call((dbl ? (uintptr_t)&floor : (uintptr_t)&floorf));
    emit_64bit_call((dbl ? (uintptr_t)&__fixdfdi : (uintptr_t)&__fixsfdi));
    int addr = 5;
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_64);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_STW(3, 0, addr);
    EMIT_MTLR(0);
    EMIT_STW(4, 4, addr);
    return CONVERT_SUCCESS;
}

static int ROUND_W_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    set_rounding(PPC_ROUNDING_NEAREST);
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR(fd);
    int addr = mapRegisterTemp();
    EMIT_FCTIW(0, fs);
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_32);
    EMIT_STFIWX(0, 0, addr);
    unmapRegisterTemp(addr);
    return CONVERT_SUCCESS;
}

static int TRUNC_W_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR(fd);
    int addr = mapRegisterTemp();
    EMIT_FCTIWZ(0, fs);
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_32);
    EMIT_STFIWX(0, 0, addr);
    unmapRegisterTemp(addr);
    return CONVERT_SUCCESS;
}

static int CEIL_W_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    set_rounding(PPC_ROUNDING_CEIL);
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR(fd);
    int addr = mapRegisterTemp();
    EMIT_FCTIW(0, fs);
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_32);
    EMIT_STFIWX(0, 0, addr);
    unmapRegisterTemp(addr);
    set_rounding(PPC_ROUNDING_NEAREST);
    return CONVERT_SUCCESS;
}

static int FLOOR_W_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    set_rounding(PPC_ROUNDING_FLOOR);
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR(fd);
    int addr = mapRegisterTemp();
    EMIT_FCTIW(0, fs);
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_32);
    EMIT_STFIWX(0, 0, addr);
    unmapRegisterTemp(addr);
    set_rounding(PPC_ROUNDING_NEAREST);
    return CONVERT_SUCCESS;
}

static int CVT_S_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), 0 );
    EMIT_FMR(fd, fs);
    return CONVERT_SUCCESS;
}

static int CVT_D_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int fd = mapFPRNew( MIPS_GET_FD(mips), 1 );
    EMIT_FMR(fd, fs);
    return CONVERT_SUCCESS;
}

static int CVT_W_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    EMIT_LFD(0, -4, DYNAREG_FCR31);
    set_rounding_reg(0);
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR(fd);
    int addr = mapRegisterTemp();
    EMIT_FCTIW(0, fs);
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_32);
    EMIT_STFIWX(0, 0, addr);
    unmapRegisterTemp(addr);
    set_rounding(PPC_ROUNDING_NEAREST);
    return CONVERT_SUCCESS;
}

static int CVT_L_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fd = MIPS_GET_FD(mips);
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    invalidateFPR( MIPS_GET_FS(mips) );
    EMIT_FMR(1,fs);
    emit_64bit_call((dbl ? (uintptr_t)&__fixdfdi : (uintptr_t)&__fixsfdi));
    int addr = 5;
    EMIT_LWZ(addr, fd*4, DYNAREG_FPR_64);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_STW(3, 0, addr);
    EMIT_MTLR(0);
    EMIT_STW(4, 4, addr);
    return CONVERT_SUCCESS;
}

static int C_F_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_UN_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BC(2, 0, 0, 0x4, 3);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_EQ_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BNE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_UEQ_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_CROR(2, 2, 3);
    EMIT_BNE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_OLT_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_ULT_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_CROR(0, 0, 3);
    EMIT_BGE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_OLE_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_CROR(1, 1, 3);
    EMIT_BGT(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_ULE_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGT(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_SF_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_NGLE_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_SEQ_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BNE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_NGL_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BNE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_LT_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_NGE_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGE(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_LE_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGT(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int C_NGT_FP(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = mapFPR( MIPS_GET_FS(mips), dbl );
    int ft = mapFPR( MIPS_GET_FT(mips), dbl );
    EMIT_LWZ(0, 0, DYNAREG_FCR31);
    EMIT_FCMPU(fs, ft, 0);
    EMIT_RLWINM(0, 0, 0, 9, 7);
    EMIT_BGT(0, 2, 0, 0);
    EMIT_ORIS(0, 0, 0x0080);
    EMIT_STW(0, 0, DYNAREG_FCR31);
    return CONVERT_SUCCESS;
}

static int (*gen_cop1_fp[64])(MIPS_instr, int) = {
   ADD_FP    ,SUB_FP    ,MUL_FP   ,DIV_FP    ,SQRT_FP   ,ABS_FP    ,MOV_FP   ,NEG_FP    ,
   ROUND_L_FP,TRUNC_L_FP,CEIL_L_FP,FLOOR_L_FP,ROUND_W_FP,TRUNC_W_FP,CEIL_W_FP,FLOOR_W_FP,
   NI2       ,NI2       ,NI2      ,NI2       ,NI2       ,NI2       ,NI2      ,NI2       ,
   NI2       ,NI2       ,NI2      ,NI2       ,NI2       ,NI2       ,NI2      ,NI2       ,
   CVT_S_FP  ,CVT_D_FP  ,NI2      ,NI2       ,CVT_W_FP  ,CVT_L_FP  ,NI2      ,NI2       ,
   NI2       ,NI2       ,NI2      ,NI2       ,NI2       ,NI2       ,NI2      ,NI2       ,
   C_F_FP    ,C_UN_FP   ,C_EQ_FP  ,C_UEQ_FP  ,C_OLT_FP  ,C_ULT_FP  ,C_OLE_FP ,C_ULE_FP  ,
   C_SF_FP   ,C_NGLE_FP ,C_SEQ_FP ,C_NGL_FP  ,C_LT_FP   ,C_NGE_FP  ,C_LE_FP  ,C_NGT_FP
};

static int S(MIPS_instr mips){
    return gen_cop1_fp[ MIPS_GET_FUNC(mips) ](mips, 0);
}

static int D(MIPS_instr mips){
    return gen_cop1_fp[ MIPS_GET_FUNC(mips) ](mips, 1);
}

static int CVT_FP_W(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = MIPS_GET_FS(mips);
    flushFPR(fs);
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    int tmp = mapRegisterTemp();
    EMIT_LWZ(tmp, fs*4, DYNAREG_FPR_32);
    EMIT_LWZ(tmp, 0, tmp);
    EMIT_LIS(0, 0x4330);
    EMIT_STW(0, -8, 1);
    EMIT_LIS(0, 0x8000);
    EMIT_STW(0, -4, 1);
    EMIT_XOR(0, tmp, 0);
    EMIT_LFD(0, -8, 1);
    EMIT_STW(0, -4, 1);
    EMIT_LFD(fd, -8, 1);
    EMIT_FSUB(fd, fd, 0, dbl);
    unmapRegisterTemp(tmp);
    return CONVERT_SUCCESS;
}

static int W(MIPS_instr mips){
    int func = MIPS_GET_FUNC(mips);
    if(func == MIPS_FUNC_CVT_S_) return CVT_FP_W(mips, 0);
    if(func == MIPS_FUNC_CVT_D_) return CVT_FP_W(mips, 1);
    else return CONVERT_ERROR;
}

static int CVT_FP_L(MIPS_instr mips, int dbl){
    genCheckFP();
    int fs = MIPS_GET_FS(mips);
    int fd = mapFPRNew( MIPS_GET_FD(mips), dbl );
    int hi = mapRegisterTemp();
    int lo = mapRegisterTemp();
    EMIT_LWZ(lo, fs*4, DYNAREG_FPR_64);
    EMIT_LWZ(hi, 0, lo);
    EMIT_LWZ(lo, 4, lo);
    EMIT_OR(3,hi,hi);
    EMIT_OR(4,lo,lo);
    emit_64bit_call((dbl ? (uintptr_t)&__floatdidf : (uintptr_t)&__floatdisf));
    EMIT_FMR(fd,1);
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_MTLR(0);
    unmapRegisterTemp(hi);
    unmapRegisterTemp(lo);
    return CONVERT_SUCCESS;
}

static int L(MIPS_instr mips){
    int func = MIPS_GET_FUNC(mips);
    if(func == MIPS_FUNC_CVT_S_) return CVT_FP_L(mips, 0);
    if(func == MIPS_FUNC_CVT_D_) return CVT_FP_L(mips, 1);
    else return CONVERT_ERROR;
}

static int (*gen_cop1[32])(MIPS_instr) = {
   MFC1, DMFC1, CFC1, NI, MTC1, DMTC1, CTC1, NI,
   BC  , NI   , NI  , NI, NI  , NI   , NI  , NI,
   S   , D    , NI  , NI, W   , L    , NI  , NI,
   NI  , NI   , NI  , NI, NI  , NI   , NI  , NI
};

static int COP1(MIPS_instr mips){
    return gen_cop1[MIPS_GET_RS(mips)](mips);
}

static int (*gen_ops[64])(MIPS_instr) = {
   SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ , BGTZ ,
   ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI , LUI  ,
   COP0   , COP1  , NI  , NI   , BEQL, BNEL, BLEZL, BGTZL,
   DADDI  , DADDIU, LDL , LDR  , NI  , NI  , NI   , NI   ,
   LB     , LH    , LWL , LW   , LBU , LHU , LWR  , LWU  ,
   SB     , SH    , SWL , SW   , SDL , SDR , SWR  , CACHE,
   LL     , LWC1  , NI  , NI   , NI  , LDC1, NI   , LD   ,
   SC     , SWC1  , NI  , NI   , NI  , SDC1, NI   , SD
};

static void genCallInterp(MIPS_instr mips){
    flushRegisters();
    reset_code_addr();
    EMIT_LI(5, isDelaySlot ? 1 : 0);
    EMIT_LIS(3, mips>>16);
    EMIT_LIS(4, get_src_pc()>>16);
    EMIT_ORI(3, 3, mips);
    EMIT_ORI(4, 4, get_src_pc());
    emit_64bit_call((uintptr_t)(&decodeNInterpret));
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_CMPI(3, 0, 6);
    EMIT_MTLR(0);
    EMIT_BNELR(6, 0);
    if(mips_is_jump(mips)) delaySlotNext = 2;
}

static void genJumpTo(unsigned int loc, unsigned int type){
    if(type == JUMPTO_REG){
        EMIT_LWZ(3, loc*8+4, DYNAREG_REG);
    } else {
        loc <<= 2;
        if(type == JUMPTO_OFF) loc += get_src_pc();
        else loc |= get_src_pc() & 0xf0000000;
        EMIT_ORI(0, 0, 0);
        EMIT_ORI(0, 0, 0);
        EMIT_ORI(0, 0, 0);
        EMIT_STW(3, 0, DYNAREG_FUNC);
        EMIT_ADDI(3, 3, 1);
        EMIT_LIS(3, loc >> 16);
        EMIT_ORI(3, 3, loc);
        EMIT_BLELR(2, 0);
        EMIT_STW(3, 0, DYNAREG_LADDR);
    }
    EMIT_BLR((type != JUMPTO_REG));
}

static void genUpdateCount(int checkCount){
    int tmp = mapRegisterTemp();
    EMIT_LIS(tmp, (get_src_pc()+4)>>16);
    EMIT_LWZ(0, 0, DYNAREG_LADDR);
    EMIT_ORI(tmp, tmp, get_src_pc()+4);
    EMIT_STW(tmp, 0, DYNAREG_LADDR);
    EMIT_SUBF(0, 0, tmp);
    EMIT_LWZ(tmp, 9*4, DYNAREG_COP0);
    EMIT_SRWI(0, 0, 1);
    EMIT_ADD(0, 0, tmp);
    if(checkCount){
        EMIT_LWZ(tmp, 0, DYNAREG_NINTR);
    }
    EMIT_STW(0, 9*4, DYNAREG_COP0);
    if(checkCount){
        EMIT_CMPL(tmp, 0, 2);
    }
    unmapRegisterTemp(tmp);
}

static void genCheckFP(void){
    if(FP_need_check || isDelaySlot){
        flushRegisters();
        reset_code_addr();
        EMIT_LWZ(0, 12*4, DYNAREG_COP0);
        EMIT_ANDIS(0, 0, 0x2000);
        EMIT_BNE(0, 8, 0, 0);
        EMIT_LIS(3, get_src_pc()>>16);
        EMIT_LI(4, isDelaySlot ? 1 : 0);
        EMIT_ORI(3, 3, get_src_pc());
        emit_64bit_call((uintptr_t)(&dyna_check_cop1_unusable));
        EMIT_LD(0, DYNAOFF_LR, 1);
        EMIT_MTLR(0);
        EMIT_BLR(0);
        FP_need_check = isDelaySlot;
    }
}

static int mem_call_seq = 0;

void genCallDynaMem(memType type, int base, short immed){
    if (mem_call_seq == 0) {
        EMIT_LI(0, 0xCC);
        EMIT_STW(0, 40, 31);      /* canary[10] = 0xCC (1st call, before dyna_mem) */
    } else {
        EMIT_LI(0, 0xEE);
        EMIT_STW(0, 44, 31);      /* canary[11] = 0xEE (subseq call, before dyna_mem) */
    }
    mem_call_seq++;

    EMIT_LIS(6, (get_src_pc()+4)>>16);
    EMIT_ADDI(4, base, immed);
    EMIT_LI(5, type);
    EMIT_ORI(6, 6, get_src_pc()+4);
    EMIT_LI(7, isDelaySlot ? 1 : 0);
    emit_64bit_call((uintptr_t)(&dyna_mem));

    if (mem_call_seq == 1) {
        EMIT_LI(0, 0xDD);
        EMIT_STW(0, 44, 31);      /* canary[11] = 0xDD (1st call, returned from dyna_mem) */
    }
    EMIT_LD(0, DYNAOFF_LR, 1);
    EMIT_CMPI(3, 0, 6);
    EMIT_MTLR(0);
    EMIT_BNELR(6, 0);
}

static int genCallDynaMemVM(int rs_reg, int rt_reg, memType type, int immed){
    PowerPC_instr* preCall=NULL;
    int not_fastmem_id=0;

    if(type==MEM_LWC1 || type==MEM_LDC1 || type==MEM_SWC1 || type==MEM_SDC1){
        genCheckFP();
        if(type==MEM_SWC1 || type==MEM_SDC1){
            flushFPR(rt_reg);
        } else {
            invalidateFPR(rt_reg);
        }
    }

    int base = mapRegister( rs_reg );

    if(!(failsafeRec&FAILSAFE_REC_NO_VM)){
        int rd = 5;
        int addr = 6;

        EMIT_RLWINM(rd,base,0,2,31);
        EMIT_ORIS(rd,rd,0x4000);

        switch (type){
            case MEM_LB: {
                int r = mapRegisterNew( rt_reg );
                EMIT_LBZ(r, immed, rd);
                EMIT_EXTSB(r,r);
                break;
            }
            case MEM_LBU: {
                int r = mapRegisterNew( rt_reg );
                EMIT_LBZ(r, immed, rd);
                break;
            }
            case MEM_LH: {
                int r = mapRegisterNew( rt_reg );
                EMIT_LHA(r, immed, rd);
                break;
            }
            case MEM_LHU: {
                int r = mapRegisterNew( rt_reg );
                EMIT_LHZ(r, immed, rd);
                break;
            }
            case MEM_LW: {
                int r = mapRegisterNew( rt_reg );
                EMIT_LWZ(r, immed, rd);
                break;
            }
            case MEM_LWU: {
                RegMapping r = mapRegister64New( rt_reg );
                EMIT_LWZ(r.lo, immed, rd);
                EMIT_LI(r.hi, 0);
                break;
            }
            case MEM_LD: {
                RegMapping r = mapRegister64New( rt_reg );
                EMIT_LWZ(r.hi, immed, rd);
                EMIT_LWZ(r.lo, immed+4, rd);
                break;
            }
            case MEM_LWC1: {
                int r = 3;
                EMIT_LWZ(r, immed, rd);
                EMIT_LWZ(addr, rt_reg*4, DYNAREG_FPR_32);
                EMIT_STW(r, 0, addr);
                break;
            }
            case MEM_LDC1: {
                int r = 3;
                int r2 = 4;
                EMIT_LWZ(r, immed, rd);
                EMIT_LWZ(r2, immed+4, rd);
                EMIT_LWZ(addr, rt_reg*4, DYNAREG_FPR_64);
                EMIT_STW(r, 0, addr);
                EMIT_STW(r2, 4, addr);
                break;
            }
            case MEM_LWL: {
                EMIT_ADDI(rd, rd, immed);
                EMIT_RLWINM(0, rd, 0, 30, 31);
                EMIT_CMPI(0, 0, 1);
                EMIT_BNE(1, 3, 0, 0);
                int r = mapRegisterNew( rt_reg );
                EMIT_LWZ(r, 0, rd);
                break;
            }
            case MEM_LWR: {
                EMIT_ADDI(rd, rd, immed);
                EMIT_RLWINM(0, rd, 0, 30, 31);
                EMIT_CMPI(0, 3, 1);
                EMIT_BNE(1, 4, 0, 0);
                EMIT_RLWINM(rd, rd, 0, 0, 29);
                int r = mapRegisterNew( rt_reg );
                EMIT_LWZ(r, 0, rd);
                break;
            }
            case MEM_SB: {
                int r = mapRegister( rt_reg );
                EMIT_STB(r, immed, rd);
                break;
            }
            case MEM_SH: {
                int r = mapRegister( rt_reg );
                EMIT_STH(r, immed, rd);
                break;
            }
            case MEM_SW: {
                int r = mapRegister( rt_reg );
                EMIT_STW(r, immed, rd);
                break;
            }
            case MEM_SD: {
                RegMapping r = mapRegister64( rt_reg );
                EMIT_STW(r.hi, immed, rd);
                EMIT_STW(r.lo, immed+4, rd);
                break;
            }
            case MEM_SWC1: {
                int r = 3;
                EMIT_LWZ(addr, rt_reg*4, DYNAREG_FPR_32);
                EMIT_LWZ(r, 0, addr);
                EMIT_STW(r, immed, rd);
                break;
            }
            case MEM_SDC1: {
                int r = 3;
                int r2 = 4;
                EMIT_LWZ(addr, rt_reg*4, DYNAREG_FPR_64);
                EMIT_LWZ(r, 0, addr);
                EMIT_LWZ(r2, 4, addr);
                EMIT_STW(r, immed, rd);
                EMIT_STW(r2, immed+4, rd);
                break;
            }
            default:
                assert(0);
        }

        not_fastmem_id = add_jump_special(1);
        EMIT_B(not_fastmem_id, 0, 0);
        preCall = get_curr_dst();
    }

    if(type!=MEM_SW && type!=MEM_SH && type!=MEM_SB){
        EMIT_LI(3, rt_reg);
    } else {
        int r=mapRegister( rt_reg );
        if(r!=3)
            EMIT_OR(3,r,r);
    }

    genCallDynaMem(type, base, immed);

    if(!(failsafeRec&FAILSAFE_REC_NO_VM)){
        if(type==MEM_LW || type==MEM_LH || type==MEM_LB || type==MEM_LHU || type==MEM_LBU || type==MEM_LWL || type==MEM_LWR){
            reloadRegister( rt_reg );
        } else if(type==MEM_LWU || type==MEM_LD){
            reloadRegister64( rt_reg );
        }

        int callSize = get_curr_dst() - preCall;
        set_jump_special(not_fastmem_id, callSize+1);
    } else {
        invalidateRegisters();
    }

    return CONVERT_SUCCESS;
}

void * rewriteDynaMemVM(void* fault_addr){
    (void)fault_addr;
    return NULL;
}
