#ifndef __INCLUDE_NUTTX_MM_MEMCHECKER_H
#define __INCLUDE_NUTTX_MM_MEMCHECKER_H

#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#ifndef __ASSEMBLY__

FAR void *memchecker_malloc(const char *file, int line, size_t size);
void memchecker_free(const char *file, int line, FAR const void *ptr);

#define malloc(size) memchecker_malloc(__FILE__, __LINE__, size)
#define free(ptr) memchecker_free(__FILE__, __LINE__, ptr)

#endif

#define MEMCHECKER_STACK_DEPTH 32

#define MEMCHECKER_CANARY_PATTERN(addr) ((uint8_t)0xa3 ^ (uint8_t)((unsigned long)(addr) & 0x7))

enum memchecker_state
{
    MEMCHECKER_MEMORY_UNUSED,
    MEMCHECKER_MEMORY_ALLOCATED,
    MEMCHECKER_MEMORY_FREED,
};

enum memchecker_error_type
{
    MEMCHECKER_ERROR_OUT_OF_MEMORY,
    MEMCHECKER_ERROR_DOUBLE_FREE,
    MEMCHECKER_ERROR_USE_AFTER_FREE,
    MEMCHECKER_ERROR_INVALID_FREE,
};

struct memchecker_metadata
{
	struct list_node list;
	
	enum memchecker_state state;
	
    unsigned long addr;
    
    size_t size;
    
    enum mc_state_e state;
};

void memchecker_init(void);

char *get_mc_pool(void);

#endif /* __INCLUDE_NUTTX_MM_MEMCHECKER_H */

