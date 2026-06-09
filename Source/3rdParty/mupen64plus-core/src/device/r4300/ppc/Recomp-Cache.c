#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Recompile.h"
#include "Wrappers.h"
#include "Recomp-Cache.h"

void DCFlushRange(void* startaddr, unsigned int len)
{
    unsigned long start = (unsigned long)startaddr & ~31UL;
    unsigned long end = (unsigned long)startaddr + len;
    for (; start < end; start += 32)
        __asm__ __volatile__("dcbst 0,%0" :: "r"(start) : "memory");
    __asm__ __volatile__("sync" ::: "memory");
}

void ICInvalidateRange(void* startaddr, unsigned int len)
{
    unsigned long start = (unsigned long)startaddr & ~31UL;
    unsigned long end = (unsigned long)startaddr + len;
    for (; start < end; start += 32)
        __asm__ __volatile__("icbi 0,%0" :: "r"(start) : "memory");
    __asm__ __volatile__("sync" ::: "memory");
    __asm__ __volatile__("isync" ::: "memory");
}

static void* recompmeta_buf = NULL;

typedef struct _meta_node {
    unsigned int  addr;
    PowerPC_func* func;
    unsigned int  size;
} CacheMetaNode;

static void* cache_mem = NULL;
static void* meta_cache_mem = NULL;
static int cacheSize = 0;

#define HEAP_CHILD1(i) ((i<<1)+1)
#define HEAP_CHILD2(i) ((i<<1)+2)
#define HEAP_PARENT(i) ((i-1)>>1)

#define INITIAL_HEAP_SIZE (64)
static unsigned int heapSize = 0;
static unsigned int maxHeapSize = 0;
static CacheMetaNode** cacheHeap = NULL;

static void heapSwap(int i, int j){
    CacheMetaNode* t = cacheHeap[i];
    cacheHeap[i] = cacheHeap[j];
    cacheHeap[j] = t;
}

static void heapUp(int i){
    while(i && cacheHeap[i]->func->lru < cacheHeap[HEAP_PARENT(i)]->func->lru){
        heapSwap(i, HEAP_PARENT(i));
        i = HEAP_PARENT(i);
    }
}

static void heapDown(int i){
    while(1){
        unsigned int lru = cacheHeap[i]->func->lru;
        CacheMetaNode* c1 = (HEAP_CHILD1(i) < heapSize) ?
                             cacheHeap[HEAP_CHILD1(i)] : NULL;
        CacheMetaNode* c2 = (HEAP_CHILD2(i) < heapSize) ?
                             cacheHeap[HEAP_CHILD2(i)] : NULL;
        if(c1 && lru > c1->func->lru &&
           (!c2 || c1->func->lru < c2->func->lru)){
            heapSwap(i, HEAP_CHILD1(i));
            i = HEAP_CHILD1(i);
        } else if(c2 && lru > c2->func->lru){
            heapSwap(i, HEAP_CHILD2(i));
            i = HEAP_CHILD2(i);
        } else break;
    }
}

static void heapify(void){
    int i;
    for(i=1; i<heapSize; ++i) heapUp(i);
}

static void heapPush(CacheMetaNode* node){
    if(heapSize == maxHeapSize){
        maxHeapSize = 3*maxHeapSize/2 + 10;
        cacheHeap = realloc(cacheHeap, maxHeapSize*sizeof(void*));
    }
    cacheHeap[heapSize++] = node;
}

static CacheMetaNode* heapPop(void){
    heapSwap(0, --heapSize);
    heapDown(0);
    return cacheHeap[heapSize];
}

void remove_outgoing_links(PowerPC_func_node** node, PowerPC_func* func){
    if(!*node) return;
    if((*node)->left) remove_outgoing_links(&(*node)->left,func);
    if((*node)->right) remove_outgoing_links(&(*node)->right,func);
    PowerPC_func_link_node** link, ** next;
    for(link = &(*node)->function->links_in; *link != NULL; link = next){
        next = &(*link)->next;
        if((*link)->func == func){
            PowerPC_func_link_node* tmp = (*link)->next;
            MetaCache_Free(*link);
            *link = tmp;
            next = link;
        }
    }
    MetaCache_Free(*node);
}

static void unlink_func(PowerPC_func* func){
    PowerPC_func_link_node* link, * next_link;
    for(link = func->links_in; link != NULL; link = next_link){
        next_link = link->next;
        GEN_ORI(*(link->branch-10), 0, 0, 0);
        GEN_ORI(*(link->branch-9), 0, 0, 0);
        GEN_BLR(*link->branch, 1);
        DCFlushRange(link->branch-10, 11*sizeof(PowerPC_instr));
        ICInvalidateRange(link->branch-10, 11*sizeof(PowerPC_instr));
        remove_func(&link->func->links_out, func);
        MetaCache_Free(link);
    }
    func->links_in = NULL;
    remove_outgoing_links(&func->links_out, func);
    func->links_out = NULL;
}

static void free_func(PowerPC_func* func, unsigned int addr){
    func->magic = 0;
    munmap(func->code, func->code_alloc_size);
    MetaCache_Free(func->code_addr);
    PowerPC_block* block = blocks_get(addr>>12);
    if(block) remove_func(&block->funcs, func);
    unlink_func(func);
    free(func);
}

unsigned int recomp_cache_nextLRU = 0;

static inline void update_lru(PowerPC_func* func){
    func->lru = recomp_cache_nextLRU++;
    if(!recomp_cache_nextLRU || recomp_cache_nextLRU>0xf0000000){
        heapify();
        CacheMetaNode** newHeap = malloc(maxHeapSize * sizeof(CacheMetaNode*));
        int i, savedSize = heapSize;
        for(i=0; heapSize > 0; ++i){
            newHeap[i] = heapPop();
            newHeap[i]->func->lru = i;
        }
        free(cacheHeap);
        cacheHeap = newHeap;
        recomp_cache_nextLRU = heapSize = savedSize;
    }
}

static void release(int minNeeded){
    int toFree = minNeeded * 2;
    heapify();
    while(toFree > 0 && cacheSize){
        CacheMetaNode* n = heapPop();
        free_func(n->func, n->addr);
        toFree    -= n->size;
        cacheSize -= n->size;
        free(n);
    }
}

void RecompCache_Alloc(unsigned int size, unsigned int address, PowerPC_func* func){
    CacheMetaNode* newBlock = malloc( sizeof(CacheMetaNode) );
    newBlock->addr = address;
    newBlock->size = size;
    newBlock->func = func;

    long pagesize = sysconf(_SC_PAGESIZE);
    size_t aligned_size = ((size + pagesize - 1) / pagesize) * pagesize;
    void* code = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fprintf(stderr, "[RECOMP] mmap code=%p size=%zu pagesize=%ld aligned=%zu prot=0x%x addr=0x%08X\n",
            code, size, pagesize, aligned_size,
            PROT_READ|PROT_WRITE|PROT_EXEC,
            (unsigned int)(unsigned long)func->code);
    while(code == MAP_FAILED){
        release(size);
        fprintf(stderr, "[RECOMP] mmap failed, retrying after release\n");
        code = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    func->code_alloc_size = aligned_size;
    int num_instrs = (func->end_address - func->start_address + 4) >> 2;
    void* code_addr = MetaCache_Alloc(num_instrs * sizeof(void*));

    cacheSize += size;
    newBlock->func->code = code;
    newBlock->func->code_addr = code_addr;
    heapPush(newBlock);
    update_lru(func);
}

void RecompCache_Free(unsigned int addr){
    int i;
    CacheMetaNode* n = NULL;
    for(i=heapSize-1; i>=0; --i){
        if(cacheHeap[i]->addr == addr){
            n = cacheHeap[i];
            heapSwap(i, --heapSize);
            free_func(n->func, addr);
            cacheSize -= n->size;
            free(n);
            return;
        }
    }
}

void RecompCache_Update(PowerPC_func* func){
    update_lru(func);
}

void RecompCache_Link(PowerPC_func* src_func, PowerPC_instr* src_instr,
                      PowerPC_func* dst_func, PowerPC_instr* dst_instr){
    PowerPC_func_link_node* fln =
        MetaCache_Alloc(sizeof(PowerPC_func_link_node));
    fln->branch = src_instr;
    fln->func = src_func;
    fln->next = dst_func->links_in;
    dst_func->links_in = fln;
    insert_func(&src_func->links_out, dst_func);

    /* Load dst_func address (64-bit on PPC64) into DYNAREG_FUNC */
    uint64_t df_addr = (uint64_t)dst_func;
    GEN_LIS(*(src_instr-10), DYNAREG_FUNC, (df_addr >> 48) & 0xFFFF);
    GEN_ORI(*(src_instr-9), DYNAREG_FUNC, DYNAREG_FUNC, (df_addr >> 32) & 0xFFFF);
    GEN_RLDICR(*(src_instr-8), DYNAREG_FUNC, DYNAREG_FUNC, 32, 31, 0);
    GEN_ORIS(*(src_instr-7), DYNAREG_FUNC, DYNAREG_FUNC, (df_addr >> 16) & 0xFFFF);
    GEN_ORI(*(src_instr-6), DYNAREG_FUNC, DYNAREG_FUNC, df_addr & 0xFFFF);
    GEN_B(*src_instr, (PowerPC_instr*)dst_instr-src_instr, 0, 0);
    DCFlushRange(src_instr-10, 11*sizeof(PowerPC_instr));
    ICInvalidateRange(src_instr-10, 11*sizeof(PowerPC_instr));
}

unsigned char recomp_cache_buffer[RECOMP_CACHE_SIZE]
    __attribute__((aligned(65536)));

void RecompCache_Init(void){
    if(!cache_mem) {
        cache_mem = malloc(RECOMP_CACHE_SIZE);
    }
    if(!recompmeta_buf) {
        recompmeta_buf = malloc(RECOMPMETA_SIZE);
    }
    if(!meta_cache_mem) {
        meta_cache_mem = malloc(RECOMPMETA_SIZE);
    }
}

unsigned int RecompCache_Allocated(void)
{
    return cacheSize;
}

void* MetaCache_Alloc(unsigned int size){
    return malloc(size);
}

void MetaCache_Free(void* ptr){
    free(ptr);
}
