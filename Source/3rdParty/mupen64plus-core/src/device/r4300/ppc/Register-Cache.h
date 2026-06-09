#ifndef REGISTER_CACHE_H
#define REGISTER_CACHE_H

typedef struct { int hi, lo; } RegMapping;
typedef enum { MAPPING_NONE, MAPPING_32, MAPPING_64 } RegMappingType;

int mapRegister(int reg);
int mapRegisterNew(int reg);
RegMapping mapRegister64(int reg);
RegMapping mapRegister64New(int reg);
void invalidateRegister(int reg);
void flushRegister(int reg);
RegMappingType getRegisterMapping(int reg);

void reloadRegister(int reg);
void reloadRegister64(int reg);

int mapFPR(int fpr, int dbl);
int mapFPRNew(int fpr, int dbl);
void invalidateFPR(int fpr);
void flushFPR(int fpr);

int flushRegisters(void);
void invalidateRegisters(void);
int mapRegisterTemp(void);
void unmapRegisterTemp(int tmp);
int mapFPRTemp(void);
void unmapFPRTemp(int tmp);

#endif
