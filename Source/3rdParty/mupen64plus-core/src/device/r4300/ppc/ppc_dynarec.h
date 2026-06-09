#ifndef M64P_DEVICE_R4300_PPC_DYNAREC_H
#define M64P_DEVICE_R4300_PPC_DYNAREC_H

#include <stddef.h>
#include <stdint.h>

struct r4300_core;

void ppc_dynarec_init(struct r4300_core* r4300);
void ppc_dynarec_start(struct r4300_core* r4300);
void ppc_dynarec_cleanup(void);
void invalidate_cached_code_ppc(struct r4300_core* r4300, uint32_t address, size_t size);
void ppc_dynarec_jump_to(struct r4300_core* r4300, uint32_t address);

#endif
