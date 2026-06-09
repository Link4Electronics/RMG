#ifndef RECOMP_CACHE_H
#define RECOMP_CACHE_H

#include "Recompile.h"

#define RECOMP_CACHE_SIZE (24*1024*1024)
#define RECOMPMETA_SIZE (8*1024*1024)

extern unsigned char recomp_cache_buffer[RECOMP_CACHE_SIZE]
    __attribute__((aligned(65536)));

extern unsigned int recomp_cache_nextLRU;

void DCFlushRange(void* startaddr, unsigned int len);
void ICInvalidateRange(void* startaddr, unsigned int len);

void RecompCache_Alloc(unsigned int size, unsigned int address, PowerPC_func* func);
void RecompCache_Free(unsigned int addr);
void RecompCache_Update(PowerPC_func* func);
void RecompCache_Link(PowerPC_func* src_func, PowerPC_instr* src_instr,
                      PowerPC_func* dst_func, PowerPC_instr* dst_instr);

void RecompCache_Init(void);
unsigned int RecompCache_Allocated(void);
void* MetaCache_Alloc(unsigned int size);
void MetaCache_Free(void* ptr);

#endif
