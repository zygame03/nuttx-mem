#ifndef __INCLUDE_NUTTX_MM_MEMCHECKER_H
#define __INCLUDE_NUTTX_MM_MEMCHECKER_H

#include <nuttx/list.h>
#include <nuttx/spinlock.h>
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

#endif /* __ASSEMBLY__ */

#define MEMCHECKER_STACK_DEPTH 32

#define MAX_TASKS 16

#define BYTECHECKER_CANARY_PATTERN(addr) \
  ((uint8_t)0xa3 ^ (uint8_t)((unsigned long)(addr) & 0x7))

enum memchecker_state
{
  MEMCHECKER_UNUSED,
  MEMCHECKER_ALLOCATED,
  MEMCHECKER_FREED,
  MEMCHECKER_ERROR,
};

enum memchecker_error_type
{
  ERROR_NO_ERROR,
  ERROR_OUT_OF_BOUDNDS,
  ERROR_USE_AFTER_FREE,
  ERROR_INVALID_FREE,
  ERROR_MEMORY_LEAK,
};

struct memchecker_track {
  char file[32];
  int line;
	int cpu;
	uint32_t ts;
	int num_stack_entries;
	unsigned long stack_entries[MEMCHECKER_STACK_DEPTH];
};

struct memchecker_metadata
{
  struct list_node node;

  unsigned long addr;

  enum memchecker_state state;

  enum memchecker_error_type error_type;

  spinlock_t lock;

  size_t size;

  pid_t pid;

  struct memchecker_track alloc_track;

  struct memchecker_track free_track;
};

void memchecker_init(void);

struct memchecker_metadata *index_to_metadata(int index);

struct memchecker_metadata *addr_to_metadata(unsigned long addr);

int pid_to_metadata(pid_t pid, struct memchecker_metadata **buffer);

int get_active_size_multi_time(pid_t pid);

#endif /* __INCLUDE_NUTTX_MM_MEMCHECKER_H */
