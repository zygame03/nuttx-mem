#ifndef __INCLUDE_NUTTX_MM_MEMCHECKER_H
#define __INCLUDE_NUTTX_MM_MEMCHECKER_H

#include <nuttx/list.h>
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

#define MEMCHECKER_CANARY 4

#define BYTECHECKER_CANARY_PATTERN(addr) \
  ((uint8_t)0xa3 ^ (uint8_t)((unsigned long)(addr) & 0x7))

enum memchecker_state
{
  MEMCHECKER_ALLOCATED,
  MEMCHECKER_FREED,
  MEMCHECKER_ERROR,
};

enum memchecker_error_type
{
  ERROR_OUT_OF_BOUDNDS,
  ERROR_USE_AFTER_FREE,
  ERROR_DOUBLE_FREE,
  ERROR_INVALID_FREE,
};


struct memchecker_metadata
{
	struct list_node node;

  size_t size;
  
  unsigned long addr;
	
	enum memchecker_state state;
	
	enum memchecker_error_type error_type;
};

void print_metadata_info(void);

void memchecker_init(void);


#endif /* __INCLUDE_NUTTX_MM_MEMCHECKER_H */

