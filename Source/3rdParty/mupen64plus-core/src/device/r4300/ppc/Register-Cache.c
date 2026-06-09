#include <string.h>
#include "Register-Cache.h"
#include "PowerPC.h"
#include "Wrappers.h"

static struct {
    RegMapping map;
    int dirty;
    int lru;
} regMap[34];

static unsigned int nextLRUVal;
static int availableRegsDefault[32] = {
    /* r0=r1 reserved, r2=TOC(r2) reserved on PPC64, r3-r12 arg/scratch,
       r13=small-data(thread ptr) reserved, r14-r23=DYNAREG fixed registers,
       r24-r31 general purpose */
    0,0,0, 0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1
    };
static int availableRegs[32];

static void _flushRegister(int reg){
    if(regMap[reg].map.hi >= 0){
        EMIT_STW(regMap[reg].map.hi, reg*8, DYNAREG_REG);
    } else {
        EMIT_SRAWI(0, regMap[reg].map.lo, 31);
        EMIT_STW(0, reg*8, DYNAREG_REG);
    }
    EMIT_STW(regMap[reg].map.lo, reg*8+4, DYNAREG_REG);
}

static int getAvailableHWReg(void){
    int i;
    for(i=0; i<32; ++i){
        if(availableRegs[i]){
            availableRegs[i] = 0;
            return i;
        }
    }
    return -1;
}

static RegMapping flushLRURegister(void){
    int i, lru_i = 0, lru_v = 0x7fffffff;
    for(i=1; i<34; ++i){
        if(regMap[i].map.lo >= 0 && regMap[i].lru < lru_v){
            lru_i = i; lru_v = regMap[i].lru;
        }
    }
    RegMapping map = regMap[lru_i].map;
    if(regMap[lru_i].dirty) _flushRegister(lru_i);
    regMap[lru_i].map.hi = regMap[lru_i].map.lo = -1;
    return map;
}

int mapRegisterNew(int reg){
    if(!reg) return 0;
    regMap[reg].lru = nextLRUVal++;
    regMap[reg].dirty = 1;
    if(regMap[reg].map.lo >= 0){
        if(regMap[reg].map.hi >= 0){
            availableRegs[regMap[reg].map.hi] = 1;
            regMap[reg].map.hi = -1;
        }
        return regMap[reg].map.lo;
    }
    int available = getAvailableHWReg();
    if(available >= 0) return regMap[reg].map.lo = available;
    RegMapping lru = flushLRURegister();
    if(lru.hi >= 0) availableRegs[lru.hi] = 1;
    return regMap[reg].map.lo = lru.lo;
}

RegMapping mapRegister64New(int reg){
    if(!reg) return (RegMapping){ 0, 0 };
    regMap[reg].lru = nextLRUVal++;
    regMap[reg].dirty = 1;
    if(regMap[reg].map.lo >= 0){
        if(regMap[reg].map.hi < 0){
            int available = getAvailableHWReg();
            if(available >= 0) regMap[reg].map.hi = available;
            else {
                RegMapping lru = flushLRURegister();
                if(lru.hi >= 0) availableRegs[lru.hi] = 1;
                regMap[reg].map.hi = lru.lo;
            }
        }
        return regMap[reg].map;
    }
    regMap[reg].map.lo = getAvailableHWReg();
    regMap[reg].map.hi = getAvailableHWReg();
    if(regMap[reg].map.lo < 0){
        RegMapping lru = flushLRURegister();
        if(lru.hi >= 0) regMap[reg].map.hi = lru.hi;
        regMap[reg].map.lo = lru.lo;
    }
    if(regMap[reg].map.hi < 0){
        RegMapping lru = flushLRURegister();
        if(lru.hi >= 0) availableRegs[lru.hi] = 1;
        regMap[reg].map.hi = lru.lo;
    }
    return regMap[reg].map;
}

int mapRegister(int reg){
    if(!reg) return DYNAREG_ZERO;
    regMap[reg].lru = nextLRUVal++;
    if(regMap[reg].map.lo >= 0){
        return regMap[reg].map.lo;
    }
    regMap[reg].dirty = 0;
    int available = getAvailableHWReg();
    if(available >= 0){
        EMIT_LWZ(available, reg*8+4, DYNAREG_REG);
        return regMap[reg].map.lo = available;
    }
    RegMapping lru = flushLRURegister();
    if(lru.hi >= 0) availableRegs[lru.hi] = 1;
    EMIT_LWZ(lru.lo, reg*8+4, DYNAREG_REG);
    return regMap[reg].map.lo = lru.lo;
}

void reloadRegister(int reg){
    if(regMap[reg].map.lo >= 0){
        EMIT_LWZ(regMap[reg].map.lo, reg*8+4, DYNAREG_REG);
    }
}

RegMapping mapRegister64(int reg){
    if(!reg) return (RegMapping){ DYNAREG_ZERO, DYNAREG_ZERO };
    regMap[reg].lru = nextLRUVal++;
    if(regMap[reg].map.lo >= 0){
        if(regMap[reg].map.hi < 0){
            int available = getAvailableHWReg();
            if(available >= 0) regMap[reg].map.hi = available;
            else {
                RegMapping lru = flushLRURegister();
                if(lru.hi >= 0) availableRegs[lru.hi] = 1;
                regMap[reg].map.hi = lru.lo;
            }
            EMIT_SRAWI(regMap[reg].map.hi, regMap[reg].map.lo, 31);
        }
        return regMap[reg].map;
    }
    regMap[reg].dirty = 0;
    regMap[reg].map.lo = getAvailableHWReg();
    regMap[reg].map.hi = getAvailableHWReg();
    if(regMap[reg].map.lo < 0){
        RegMapping lru = flushLRURegister();
        if(lru.hi >= 0) regMap[reg].map.hi = lru.hi;
        regMap[reg].map.lo = lru.lo;
    }
    if(regMap[reg].map.hi < 0){
        RegMapping lru = flushLRURegister();
        if(lru.hi >= 0) availableRegs[lru.hi] = 1;
        regMap[reg].map.hi = lru.lo;
    }
    EMIT_LWZ(regMap[reg].map.hi, reg*8, DYNAREG_REG);
    EMIT_LWZ(regMap[reg].map.lo, reg*8+4, DYNAREG_REG);
    return regMap[reg].map;
}

void reloadRegister64(int reg){
    if(regMap[reg].map.lo >= 0){
        EMIT_LWZ(regMap[reg].map.lo, reg*8+4, DYNAREG_REG);
    }
    if(regMap[reg].map.hi >= 0){
        EMIT_LWZ(regMap[reg].map.hi, reg*8, DYNAREG_REG);
    }
}

void invalidateRegister(int reg){
    if(regMap[reg].map.hi >= 0)
        availableRegs[ regMap[reg].map.hi ] = 1;
    if(regMap[reg].map.lo >= 0)
        availableRegs[ regMap[reg].map.lo ] = 1;
    regMap[reg].map.hi = regMap[reg].map.lo = -1;
}

void flushRegister(int reg){
    if(regMap[reg].map.lo >= 0){
        if(regMap[reg].dirty) _flushRegister(reg);
        if(regMap[reg].map.hi >= 0)
            availableRegs[ regMap[reg].map.hi ] = 1;
        availableRegs[ regMap[reg].map.lo ] = 1;
    }
    regMap[reg].map.hi = regMap[reg].map.lo = -1;
}

RegMappingType getRegisterMapping(int reg){
    if(regMap[reg].map.hi >= 0)
        return MAPPING_64;
    else if(regMap[reg].map.lo >= 0)
        return MAPPING_32;
    else
        return MAPPING_NONE;
}

static struct {
    int map;
    int dbl;
    int dirty;
    int lru;
} fprMap[32];

static unsigned int nextLRUValFPR;
static int availableFPRsDefault[32] = {
    0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0
    };
static int availableFPRs[32];

static void _flushFPR(int reg){
    int addr = mapRegisterTemp();
    if(fprMap[reg].dbl){
        EMIT_LWZ(addr, reg*4, DYNAREG_FPR_64);
        EMIT_STFD(fprMap[reg].map, 0, addr);
    } else {
        EMIT_LWZ(addr, reg*4, DYNAREG_FPR_32);
        EMIT_STFS(fprMap[reg].map, 0, addr);
    }
    unmapRegisterTemp(addr);
}

static int getAvailableFPR(void){
    int i;
    for(i=0; i<32; ++i){
        if(availableFPRs[i]){
            availableFPRs[i] = 0;
            return i;
        }
    }
    return -1;
}

static int flushLRUFPR(void){
    int i, lru_i = 0, lru_v = 0x7fffffff;
    for(i=0; i<32; ++i){
        if(fprMap[i].map >= 0 && fprMap[i].lru < lru_v){
            lru_i = i; lru_v = fprMap[i].lru;
        }
    }
    int map = fprMap[lru_i].map;
    if(fprMap[lru_i].dirty) _flushFPR(lru_i);
    fprMap[lru_i].map = -1;
    return map;
}

int mapFPRNew(int fpr, int dbl){
    fprMap[fpr].lru = nextLRUValFPR++;
    fprMap[fpr].dirty = 1;
    fprMap[fpr].dbl = dbl;
    if(fprMap[fpr].map >= 0) return fprMap[fpr].map;
    int available = getAvailableFPR();
    if(available >= 0) return fprMap[fpr].map = available;
    return fprMap[fpr].map = flushLRUFPR();
}

int mapFPR(int fpr, int dbl){
    fprMap[fpr].lru = nextLRUValFPR++;
    fprMap[fpr].dbl = dbl;
    if(fprMap[fpr].map >= 0) return fprMap[fpr].map;
    fprMap[fpr].dirty = 0;
    fprMap[fpr].map = getAvailableFPR();
    if(fprMap[fpr].map < 0) fprMap[fpr].map = flushLRUFPR();
    int addr = mapRegisterTemp();
    if(dbl){
        EMIT_LWZ(addr, fpr*4, DYNAREG_FPR_64);
        EMIT_LFD(fprMap[fpr].map, 0, addr);
    } else {
        EMIT_LWZ(addr, fpr*4, DYNAREG_FPR_32);
        EMIT_LFS(fprMap[fpr].map, 0, addr);
    }
    unmapRegisterTemp(addr);
    return fprMap[fpr].map;
}

void invalidateFPR(int fpr){
    if(fprMap[fpr].map >= 0)
        availableFPRs[ fprMap[fpr].map ] = 1;
    fprMap[fpr].map = -1;
}

void flushFPR(int fpr){
    if(fprMap[fpr].map >= 0){
        if(fprMap[fpr].dirty) _flushFPR(fpr);
        availableFPRs[ fprMap[fpr].map ] = 1;
    }
    fprMap[fpr].map = -1;
}

int flushRegisters(void){
    int i, flushed = 0;
    for(i=1; i<34; ++i){
        if(regMap[i].map.lo >= 0 && regMap[i].dirty){
            _flushRegister(i);
            ++flushed;
        }
        regMap[i].map.hi = regMap[i].map.lo = -1;
    }
    memcpy(availableRegs, availableRegsDefault, 32*sizeof(int));
    nextLRUVal = 0;
    for(i=0; i<32; ++i){
        if(fprMap[i].map >= 0 && fprMap[i].dirty){
            _flushFPR(i);
            ++flushed;
        }
        fprMap[i].map = -1;
    }
    memcpy(availableFPRs, availableFPRsDefault, 32*sizeof(int));
    nextLRUValFPR = 0;
    return flushed;
}

void invalidateRegisters(void){
    int i;
    for(i=0; i<34; ++i) invalidateRegister(i);
    memcpy(availableRegs, availableRegsDefault, 32*sizeof(int));
    nextLRUVal = 0;
    for(i=0; i<32; ++i) invalidateFPR(i);
    memcpy(availableFPRs, availableFPRsDefault, 32*sizeof(int));
    nextLRUValFPR = 0;
}

int mapRegisterTemp(void){
    int available = getAvailableHWReg();
    if(available >= 0) return available;
    RegMapping lru = flushLRURegister();
    if(lru.hi >= 0) availableRegs[lru.hi] = 1;
    return lru.lo;
}

void unmapRegisterTemp(int reg){
    availableRegs[reg] = 1;
}

int mapFPRTemp(void){
    int available = getAvailableFPR();
    if(available >= 0) return available;
    int lru = flushLRUFPR();
    return lru;
}

void unmapFPRTemp(int fpr){
    availableFPRs[fpr] = 1;
}
