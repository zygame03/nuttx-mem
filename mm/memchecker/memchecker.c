/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mm/memchecker.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include "hooks.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef malloc
#undef free

#define MEMCHECKER_PAGE_NUM CONFIG_MM_MEMCHECKER_PAGE_NUM
#define MEMCHECKER_PAGE_SIZE CONFIG_MM_MEMCHECKER_PAGE_SIZE
#define MEMCHECKER_POOL_SIZE (MEMCHECKER_PAGE_NUM + 1) * 2 * MEMCHECKER_PAGE_SIZE

/****************************************************************************
 * Private Types
 ****************************************************************************/

static char *_memchecker_pool; 

static struct memchecker_metadata metadata_list[MEMCHECKER_PAGE_NUM];

static struct list_node memchecker_freelist;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool is_memchecker_addr(unsigned long *addr)
{
	return ((unsigned long)((char *)addr - _memchecker_pool) <
	       MEMCHECKER_POOL_SIZE && addr);
}

static struct memchecker_metadata  *addr_to_metadata(unsigned long *addr)
{
	int index;
	if(!is_memchecker_addr((void *)addr))
	  {
		  return NULL;
	  }
	
	index = (addr - (unsigned long *)_memchecker_pool) / (MEMCHECKER_PAGE_SIZE * 2);
	if(index < 0 || index > MEMCHECKER_PAGE_NUM)
	  {
		  return NULL;
	  }
	  
	return &metadata_list[index];
}

static unsigned long metadat_to_addr(const struct memchecker_metadata *metadata)
{
	unsigned long offset = ((metadata - metadata_list) + 1) * 2 * MEMCHECKER_PAGE_SIZE;
	unsigned long addr = (unsigned long)&_memchecker_pool[offset];
	return addr;
}	

static bool set_canary_byte(uint8_t *addr)
{
  *addr = MEMCHECKER_CANARY_PATTERN(addr);
  return true;
}

static bool check_canary_byte(uint8_t *addr)
{
  if(*addr == MEMCHECKER_CANARY_PATTERN(addr))
    {
      return true;
    }
  
  return false;
}

static void for_each_canary(const struct memchecker_metadata *metadata, 
                            bool (*fn)(uint8_t *))
{
  unsigned long addr;
  enum memchecker_error_type error_type;
  
  if(metadata->state == MEMCHECKER_MEMORY_ALLOCATED)
    {
      error_type = 0;
    }
  else if(metadata->state == MEMCHECKER_MEMORY_FREED)
    {
      error_type = 1;
    }
  else
    {
      error_type = 3;
    }
  
  for(addr = metadata->addr - MEMCHECKER_PAGE_SIZE / 2; addr < metadata->addr;
      addr++)
    {
      if(!fn((uint8_t *)addr))
        {
          break;
        }
    }
  
  for(addr = metadata->addr + metadata->size;
      addr < metadata->addr + MEMCHECKER_PAGE_SIZE * 2;
      addr++)
    {
      if(!fn((uint8_t *)addr))
        {
          break;
        }
    }
}

static char *alloc_pool(void)
{

}

static void init_pool(void)
{
  
}

void *memchecker_malloc(const char *file, int line, size_t size)
{

}

void memchecker_free(const char *file, int line,const void *addr)
{

}

void memchecker_init(void)
{
    alloc_pool();
    init_pool();
}
