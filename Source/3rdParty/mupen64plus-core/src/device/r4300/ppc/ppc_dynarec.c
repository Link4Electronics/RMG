#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "device/r4300/r4300_core.h"
#include "device/r4300/cp0.h"
#include "device/r4300/cp1.h"
#include "device/r4300/tlb.h"
#include "device/memory/memory.h"
#include "device/rdram/rdram.h"
#include "main/main.h"
#include "api/callbacks.h"

#include "ppc_dynarec.h"
#include "ppc_dynarec_compat.h"
#include "Recompile.h"
#include "Recomp-Cache.h"
#include "Wrappers.h"
#include "Register-Cache.h"
#include "MIPS-to-PPC.h"

/* Global r4300 pointer for compatibility layer */
struct r4300_core* ppc_dynarec_r4300 = NULL;

/* Old-style global register arrays */
long long int reg[36];
uint32_t reg_cop0[32];
double *reg_cop1_double[32];
float *reg_cop1_simple[32];
long long int reg_cop1_fgr_64[32];
uint32_t FCR0, FCR31;
uint32_t last_addr = 0, interp_addr = 0;
uint32_t next_interupt = 0, CIC_Chip = 0;

/* PowerPC block globals */
PowerPC_block *blocks[0x100000] = {0};
PowerPC_block *actual = NULL;
char invalid_code[0x100000] = {0};

/* Wrappers globals */
int noCheckInterrupt = 0;
int failsafeRec = 0;
int llbit = 0;
uint32_t delay_slot = 0;

/* CP0 register convenience pointers */
static struct cp0* ppc_cp0 = NULL;

static PowerPC_instr* link_branch = NULL;
static PowerPC_func* last_func;

static void sync_r4300_state(struct r4300_core* r4300) {
    int i;
    for (i = 0; i < 32; i++)
        reg[i] = r4300->regs[i];
    reg[32] = r4300->hi;
    reg[33] = r4300->lo;
    for (i = 0; i < 32; i++)
        reg_cop0[i] = r4300->cp0.regs[i];
    /* Set up FPR pointers */
    for (i = 0; i < 32; i++) {
        reg_cop1_simple[i] = (float*)&r4300->cp1.regs[i];
        reg_cop1_double[i] = (double*)&r4300->cp1.regs[i];
        reg_cop1_fgr_64[i] = r4300->cp1.regs[i].dword;
    }
    FCR31 = r4300->cp1.fcr31;
    FCR0  = r4300->cp1.fcr0;
    next_interupt = r4300->cp0.next_interrupt;
    CIC_Chip = 0;
    llbit = r4300->llbit;
    delay_slot = r4300->delay_slot;
    ppc_cp0 = &r4300->cp0;
}

static void sync_back_state(struct r4300_core* r4300) {
    int i;
    for (i = 0; i < 32; i++)
        r4300->regs[i] = reg[i];
    r4300->hi = reg[32];
    r4300->lo = reg[33];
    for (i = 0; i < 32; i++)
        r4300->cp0.regs[i] = (uint32_t)reg_cop0[i];
    r4300->cp1.fcr31 = (uint32_t)FCR31;
    r4300->cp1.fcr0  = (uint32_t)FCR0;
    r4300->llbit = llbit;
    r4300->delay_slot = delay_slot;
}

unsigned int dyna_run(PowerPC_func* func, unsigned int (*code)(void)){
    unsigned int naddr;
    PowerPC_instr* return_addr;

    void* rdram_base = ppc_dynarec_r4300 && ppc_dynarec_r4300->rdram
        ? (void*)ppc_dynarec_r4300->rdram->dram : NULL;

    __asm__ volatile(
        "stdu   1, -32(1) \n"
        "mfcr   14        \n"
        "stw    14, 8(1)  \n"
        "mr     14, %9    \n"
        "mr     15, %10   \n"
        "mr     16, %11   \n"
        "mr     17, %12   \n"
        "mr     18, %13   \n"
        "mr     19, %14   \n"
        "mr     20, %15   \n"
        "mr     21, %16   \n"
        "mr     22, %17   \n"
        "addi   23, 0, 0  \n"
        "bl     .+4       \n"
        "mtctr  %0        \n"
        "mflr   4         \n"
        "addi   4, 4, 28  \n"
        "std    4, 20(1)  \n"
        "sync             \n"
        "isync            \n"
        "bctrl            \n"
        "mr     %1, 3     \n"
        "ld     %3, 20(1) \n"
        "mflr   %2        \n"
        "mr     %4, 22    \n"
        "ld     1, 0(1)   \n"
        : "=r" (naddr), "=r" (link_branch), "=r" (return_addr),
          "=r" (last_func)
        : "r" (code),
          "r" (reg), "r" (reg_cop0),
          "r" (reg_cop1_simple), "r" (reg_cop1_double),
          "r" (&FCR31), "r" (rdram_base),
          "r" (&last_addr), "r" (&next_interupt),
          "r" (func)
        : "cr0", "cr2",
            "8","9","10","11","12",
            "14","15","16","17","18","19","20","21","22","23",
            "24","25","26","27","28","29","30","31","ctr","lr",
            "%fr14","%fr15","%fr16","%fr17","%fr18","%fr19","%fr20","%fr21","%fr22","%fr23","%fr24","%fr25","%fr26","%fr27");

    link_branch = (link_branch == return_addr || link_branch == NULL) ? NULL : link_branch - 1;
    return naddr;
}

static int dbg_iter = 0;
void dynarec(unsigned int address){
    fprintf(stderr, "[PPC_DYN] dynarec() entered with address=0x%08X\n", address);
    while(1){
        if (++dbg_iter <= 50 || (dbg_iter & 0xFFF) == 0)
            fprintf(stderr, "[PPC_DYN] iter=%d address=0x%08X interp_addr=0x%08X\n",
                    dbg_iter, address, interp_addr);

        int stop_flag = 0;
        if (ppc_dynarec_r4300) {
            stop_flag = *r4300_stop(ppc_dynarec_r4300);
        }
        if (stop_flag) break;

        PowerPC_block* dst_block = blocks_get(address>>12);
        unsigned long paddr = get_physical_addr(address);

        if(paddr == 0xFFFFFFFF){
            if (dbg_iter <= 50)
                fprintf(stderr, "[PPC_DYN] TLB refill for 0x%08X\n", address);
            TLB_refill_exception(ppc_dynarec_r4300, address, 2);
            link_branch = NULL;
            address = interp_addr;
            continue;
        }

        if(!dst_block){
            dst_block = calloc(1,sizeof(PowerPC_block));
            dst_block->start_address = address & ~0xFFF;
            dst_block->end_address   = (address & ~0xFFF) + 0x1000;
            init_block(dst_block);
            blocks[address>>12] = dst_block;
            if (dbg_iter <= 50)
                fprintf(stderr, "[PPC_DYN] new block 0x%08X-0x%08X\n",
                        dst_block->start_address, dst_block->end_address);
        } else if(invalid_code[address>>12]){
            invalidate_block(dst_block);
        }

        PowerPC_func* func = find_func(&dst_block->funcs, address);

        if(!func || !func->code_addr[(address-func->start_address)>>2]){
            unsigned int saddr = address;
            if(func) {
                saddr = func->start_address;
                invalidate_func(saddr);
                dst_block->flags[(address-dst_block->start_address)>>2] |= BLOCK_FLAG_SPLIT;
            }
            if (dbg_iter <= 50)
                fprintf(stderr, "[PPC_DYN] recompiling at 0x%08X\n", saddr);
            func = recompile_block(dst_block, saddr);
            if (dbg_iter <= 50)
                fprintf(stderr, "[PPC_DYN] recompiled %slen=%u start=0x%08X end=0x%08X\n",
                        func ? "" : "NULL func ",
                        func ? func->code_length : 0,
                        func ? func->start_address : 0,
                        func ? func->end_address : 0);
            if (!func) {
                fprintf(stderr, "[PPC_DYN] FATAL: recompile_block returned NULL at 0x%08X\n", saddr);
                break;
            }
        } else {
#ifdef USE_RECOMP_CACHE
            RecompCache_Update(func);
#endif
        }

        int index = (address - func->start_address)>>2;
        unsigned int (*code)(void);
        code = (unsigned int (*)(void))func->code_addr[index];

        assert(code);

        if(!(failsafeRec & FAILSAFE_REC_NO_LINK) && link_branch &&
           last_func->magic == FUNC_MAGIC) {
            PowerPC_block *lfblk = blocks_get((last_func->end_address-4)>>12);
            if(lfblk) {
                PowerPC_func *ffunc = find_func(&lfblk->funcs, last_func->end_address-4);
                if(ffunc && ffunc == last_func) {
                    RecompCache_Link(last_func, link_branch, func, (PowerPC_instr*)code);
                }
            }
        }

        if (dbg_iter <= 50) {
            long dist = (long)((uintptr_t)&dyna_mem - (uintptr_t)code);
            fprintf(stderr, "[PPC_DYN] calling dyna_run func=0x%p code=0x%p dyna_mem=0x%p dist=%ldKB (%srange)\n",
                    (void*)func, (void*)code, (void*)&dyna_mem, dist/1024,
                    (dist < 0x2000000LL && dist > -0x2000000LL) ? "IN" : "OUT");
        }
        interp_addr = address = dyna_run(func, code);
        if (dbg_iter <= 50)
            fprintf(stderr, "[PPC_DYN] dyna_run returned naddr=0x%08X link_branch=0x%p\n",
                    interp_addr, (void*)link_branch);

        if(!noCheckInterrupt){
            last_addr = interp_addr;
            if(next_interupt <= Count){
                if (dbg_iter <= 50)
                    fprintf(stderr, "[PPC_DYN] gen_interupt next_int=%u Count=%u\n", next_interupt, Count);
                gen_interupt();
                address = interp_addr;
            }
        }
        noCheckInterrupt = 0;
    }
    interp_addr = address;
    fprintf(stderr, "[PPC_DYN] dynarec() exiting, final address=0x%08X\n", address);
}

unsigned int decodeNInterpret(MIPS_instr mips, unsigned int pc,
                              int isDelaySlot){
    fprintf(stderr, "[decodeNInterpret] op=0x%02X mips=0x%08X pc=0x%08X ds=%d\n", MIPS_GET_OPCODE(mips), mips, pc, isDelaySlot);
    unsigned int op = MIPS_GET_OPCODE(mips);
    delay_slot = isDelaySlot;
    interp_addr = pc;

    switch(op) {
    case MIPS_OPCODE_COP0: {
        unsigned int rs = MIPS_GET_RS(mips);
        unsigned int rt = MIPS_GET_RT(mips);
        unsigned int rd = MIPS_GET_RD(mips);
        if(rs >= MIPS_FRMT_MFC && rs <= MIPS_FRMT_CTC) {
            if(rs == MIPS_FRMT_MFC || rs == MIPS_FRMT_DMFC)
                reg[rt] = (long long)(int)reg_cop0[rd];
            else if(rs == MIPS_FRMT_CFC)
                reg[rt] = (long long)(int)(rd == 0 ? 0x0F000000 : FCR31);
            else if(rs == MIPS_FRMT_MTC || rs == MIPS_FRMT_DMTC)
                reg_cop0[rd] = (uint32_t)reg[rt];
            else if(rs == MIPS_FRMT_CTC) {
                if(rd == 31) FCR31 = (uint32_t)reg[rt];
            }
        } else {
            /* TLB operations */
            unsigned int func = MIPS_GET_FUNC(mips);
            struct r4300_core* r4300 = ppc_dynarec_r4300;
            if(!r4300) return 0;
            switch(func) {
            case MIPS_FUNC_TLBR:
                break;
            case MIPS_FUNC_TLBWI:
                tlb_map(&r4300->cp0.tlb, r4300->cp0.regs[CP0_INDEX_REG] & 0x3F);
                break;
            case MIPS_FUNC_TLBWR:
                r4300->cp0.regs[CP0_RANDOM_REG] = (rand() * 32) / RAND_MAX;
                tlb_map(&r4300->cp0.tlb, r4300->cp0.regs[CP0_RANDOM_REG] & 0x3F);
                break;
            case MIPS_FUNC_TLBP:
                break;
            case MIPS_FUNC_ERET:
                break;
            }
        }
        break;
    }
    case MIPS_OPCODE_CACHE:
        break;
    default:
        break;
    }
    delay_slot = 0;
    noCheckInterrupt = 0;
    return 6;
}

int dyna_update_count(unsigned int pc){
    Count += (pc - last_addr)/2;
    last_addr = pc;
    return next_interupt - Count;
}

unsigned int dyna_check_cop1_unusable(unsigned int pc, int isDelaySlot){
    delay_slot = isDelaySlot;
    interp_addr = pc;
    Cause = (11 << 2) | 0x10000000;
    if(ppc_dynarec_r4300)
        exception_general(ppc_dynarec_r4300);
    delay_slot = 0;
    noCheckInterrupt = 1;
    return interp_addr;
}

/* Called from recompiled PPC code. Must be a proper function
   (not static) since its address is embedded in the code buffer. */
void check_interupt(void){
    if(ppc_dynarec_r4300)
        gen_interrupt(ppc_dynarec_r4300);
}

void invalidate_func(unsigned int addr){
    PowerPC_block* block = blocks_get(addr>>12);
    if(!block) return;
    for(;;) {
        PowerPC_func* func = find_func(&block->funcs, addr);
        if(func)
            RecompCache_Free(func->start_address);
        else
            break;
    }
}

void check_invalidate_memory(unsigned int addr){
    if(!invalid_code[addr>>12])
        invalidate_func(addr);
}

static void read_rmg_word(uint32_t vaddr, uint32_t* val) {
    struct memory* mem = ppc_dynarec_r4300 ? ppc_dynarec_r4300->mem : NULL;
    if(!mem) return;
    const struct mem_handler* h = mem_get_handler(mem, vaddr);
    mem_read32(h, vaddr, val);
}

static void write_rmg_word(uint32_t vaddr, uint32_t val, uint32_t mask) {
    struct memory* mem = ppc_dynarec_r4300 ? ppc_dynarec_r4300->mem : NULL;
    if(!mem) return;
    const struct mem_handler* h = mem_get_handler(mem, vaddr);
    mem_write32(h, vaddr, val, mask);
}

/* Big-endian byte extraction/insertion */
#define BE_BYTE(w, b)  (((uint32_t)(w) >> (24 - ((b)<<3))) & 0xFF)
#define BE_HWORD(w, b) (((uint32_t)(w) >> (16 - ((b)<<4))) & 0xFFFF)
#define BE_INSERT_BYTE(w, val, b)  ((w) = ((w) & ~(0xFFU  << (24 - ((b)<<3)))) | (((uint32_t)(val) & 0xFFU)  << (24 - ((b)<<3))))
#define BE_INSERT_HWORD(w, val, b) ((w) = ((w) & ~(0xFFFFU << (16 - ((b)<<4)))) | (((uint32_t)(val) & 0xFFFFU) << (16 - ((b)<<4))))

static int memdbg=0;
unsigned int dyna_mem(unsigned int value, unsigned int addr,
                      memType type, unsigned int pc, int isDelaySlot){
    if (++memdbg <= 10) fprintf(stderr, "[dyna_mem] type=%d addr=0x%08X pc=0x%08X\n", type, addr, pc);
    uint32_t wval = 0;
    uint64_t dval = 0;
    interp_addr = pc;
    delay_slot = isDelaySlot;

    switch(type) {
    case MEM_LW:
        read_rmg_word(addr, &wval);
        reg[value] = (long long)(int)wval;
        break;
    case MEM_LWU:
        read_rmg_word(addr, &wval);
        reg[value] = (long long)wval;
        break;
    case MEM_LH:
        read_rmg_word(addr & ~3, &wval);
        reg[value] = (long long)(short)(uint16_t)BE_HWORD(wval, addr & 2);
        break;
    case MEM_LHU:
        read_rmg_word(addr & ~3, &wval);
        reg[value] = (long long)BE_HWORD(wval, addr & 2);
        break;
    case MEM_LB:
        read_rmg_word(addr & ~3, &wval);
        reg[value] = (long long)(signed char)BE_BYTE(wval, addr & 3);
        break;
    case MEM_LBU:
        read_rmg_word(addr & ~3, &wval);
        reg[value] = (long long)BE_BYTE(wval, addr & 3);
        break;
    case MEM_LD:
        read_rmg_word(addr, &wval);
        dval = (uint64_t)wval << 32;
        read_rmg_word(addr+4, &wval);
        dval |= wval;
        reg[value] = (long long)dval;
        break;
    case MEM_LWC1:
        read_rmg_word(addr, &wval);
        *((uint32_t*)reg_cop1_simple[value]) = wval;
        break;
    case MEM_LDC1:
        read_rmg_word(addr, &wval);
        dval = (uint64_t)wval << 32;
        read_rmg_word(addr+4, &wval);
        dval |= wval;
        *((uint64_t*)reg_cop1_double[value]) = dval;
        break;
    case MEM_LWL:
        read_rmg_word(addr & ~3, &wval);
        switch(addr&3) {
        case 0: reg[value] = (long long)(int)wval; break;
        case 1: reg[value] = (reg[value] & 0xFFLL) | ((long long)wval << 8); break;
        case 2: reg[value] = (reg[value] & 0xFFFFLL) | ((long long)wval << 16); break;
        case 3: reg[value] = (reg[value] & 0xFFFFFFLL) | ((long long)wval << 24); break;
        }
        reg[value] = (long long)(int)(reg[value] & 0xFFFFFFFF);
        break;
    case MEM_LWR:
        read_rmg_word(addr & ~3, &wval);
        switch(addr&3) {
        case 0: reg[value] = (reg[value] & ~0xFFLL) | ((wval >> 24) & 0xFF); break;
        case 1: reg[value] = (reg[value] & ~0xFFFFLL) | ((wval >> 16) & 0xFFFF); break;
        case 2: reg[value] = (reg[value] & ~0xFFFFFFLL) | ((wval >> 8) & 0xFFFFFF); break;
        case 3: reg[value] = (long long)(int)wval; break;
        }
        reg[value] = (long long)(int)(reg[value] & 0xFFFFFFFF);
        break;
    case MEM_SW:
        write_rmg_word(addr, (uint32_t)value, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    case MEM_SH: {
        uint32_t mask = 0xFFFFU << (16 - ((addr&2)<<3));
        uint32_t shifted = ((uint32_t)value & 0xFFFFU) << (16 - ((addr&2)<<3));
        read_rmg_word(addr & ~3, &wval);
        wval = (wval & ~mask) | shifted;
        write_rmg_word(addr & ~3, wval, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    }
    case MEM_SB: {
        uint32_t mask = 0xFFU << (24 - ((addr&3)<<3));
        uint32_t shifted = ((uint32_t)value & 0xFFU) << (24 - ((addr&3)<<3));
        read_rmg_word(addr & ~3, &wval);
        wval = (wval & ~mask) | shifted;
        write_rmg_word(addr & ~3, wval, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    }
    case MEM_SD: {
        uint64_t v = (uint64_t)reg[value];
        write_rmg_word(addr, (uint32_t)(v >> 32), 0xFFFFFFFF);
        write_rmg_word(addr+4, (uint32_t)v, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    }
    case MEM_SWC1: {
        uint32_t fval = *((uint32_t*)reg_cop1_simple[value]);
        write_rmg_word(addr, fval, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    }
    case MEM_SDC1: {
        uint64_t fval = *((uint64_t*)reg_cop1_double[value]);
        write_rmg_word(addr, (uint32_t)(fval >> 32), 0xFFFFFFFF);
        write_rmg_word(addr+4, (uint32_t)fval, 0xFFFFFFFF);
        check_invalidate_memory(addr);
        break;
    }
    default:
        printf("dyna_mem bad type\n");
        if (ppc_dynarec_r4300)
            *r4300_stop(ppc_dynarec_r4300) = 1;
        break;
    }
    delay_slot = 0;
    noCheckInterrupt = 0;
    return interp_addr != pc ? interp_addr : 0;
}

void ppc_dynarec_init(struct r4300_core* r4300) {
    failsafeRec |= FAILSAFE_REC_NO_VM;
    ppc_dynarec_r4300 = r4300;
    sync_r4300_state(r4300);

    RecompCache_Init();

    init_block(NULL);
    blocks[0] = calloc(1, sizeof(PowerPC_block));
    blocks[0]->start_address = 0;
    blocks[0]->end_address = 0x1000;
}

void ppc_dynarec_start(struct r4300_core* r4300) {
    sync_r4300_state(r4300);
    dynarec(r4300->start_address);
    sync_back_state(r4300);
}

void ppc_dynarec_cleanup(void) {
    int i;
    for (i = 0; i < 0x100000; i++) {
        if (blocks[i]) {
            PowerPC_func_node* node = blocks[i]->funcs;
            if (node) {
                remove_outgoing_links(&node, NULL);
                free(node);
            }
            free(blocks[i]);
            blocks[i] = NULL;
        }
    }
    ppc_dynarec_r4300 = NULL;
}

void invalidate_cached_code_ppc(struct r4300_core* r4300, uint32_t address, size_t size) {
    size_t i;
    uint32_t addr;
    uint32_t addr_max;

    if (size == 0) {
        memset(invalid_code, 1, 0x100000);
        return;
    }

    addr_max = address + size;
    for (addr = address; addr < addr_max; addr += 4) {
        i = (addr >> 12);
        if (invalid_code[i] == 0) {
            PowerPC_block* block = blocks_get(i);
            if (block == NULL || find_func(&block->funcs, addr & ~0xFFF) != NULL) {
                invalid_code[i] = 1;
                addr &= ~0xFFF;
                addr |= 0xFFC;
            }
        } else {
            addr &= ~0xFFF;
            addr |= 0xFFC;
        }
    }
}

void ppc_dynarec_jump_to(struct r4300_core* r4300, uint32_t address) {
    *r4300_stop(r4300) = 1;
}
