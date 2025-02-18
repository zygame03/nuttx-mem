/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <nuttx/mm/memchecker.h>
#include <syslog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef malloc
#undef free

/****************************************************************************
 * Private Types
 ****************************************************************************/

static struct list_node metadata_list;

static struct list_node metadata_freelist;

static struct list_node metadata_errorlist;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static struct memchecker_metadata *addr_to_metadata(unsigned long addr)
{
  struct memchecker_metadata *metadata;
  list_for_every_entry(&metadata_list, metadata,
                       struct memchecker_metadata, node)
    {
      // printf("addr: %p\n", metadata);
      if (metadata->addr == addr)
        {
          return metadata;
        }
    }
  return NULL;
}

static bool set_canary_byte(uint8_t *addr)
{
	*addr = BYTECHECKER_CANARY_PATTERN(addr);
	return true;
}

static bool check_canary_byte(uint8_t *addr)
{
	if (*addr == BYTECHECKER_CANARY_PATTERN(addr))
	  {
		  return true;
    }
	return false;
}

static void for_each_canary(struct memchecker_metadata *metadata, 
                            bool (*fn)(uint8_t *))
{
	unsigned long addr;
	
	for (addr = metadata->addr - MEMCHECKER_CANARY;
	     addr < metadata->addr; addr++)
	  {
		  if (!fn((uint8_t *)addr))
		    {
		      metadata->state = MEMCHECKER_ERROR;
		      metadata->error_type = ERROR_OUT_OF_MEMORY;
		    	break;
       	}
	  }
  
  if(metadata->state == MEMCHECKER_FREED)
    {
      for (addr = metadata->addr;addr < metadata->addr + metadata->size;
           addr++)
	      {
	        if (!fn((uint8_t *)addr))
	          {
	            metadata->state = MEMCHECKER_ERROR;
	            metadata->error_type = ERROR_USE_AFTER_FREE;
			        break;
		        }
      	}
    }
  
	for (addr = metadata->addr + metadata->size;
	     addr < metadata->addr + metadata->size + MEMCHECKER_CANARY; addr++)
	  {
	    if (!fn((uint8_t *)addr))
	      {
	        metadata->state = MEMCHECKER_ERROR;
	        metadata->error_type = ERROR_OUT_OF_MEMORY;
			    break;
		    }
  	}
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: memchecker_malloc
 * 
 * Description:
 *   Allocate memory with memory checker.
 * 
 * Input Parameters:
 *   file - The file name of the caller.
 *   line - The line number of the caller.
 *   size - The size of the memory to be allocated.
 * 
 * Returned Value:
 *   On success, a pointer to the allocated memory is returned.
 *   On failure, NULL is returned.
 ****************************************************************************/

void *memchecker_malloc(const char *file, int line, size_t size)
{
  struct memchecker_metadata *metadata =
    (struct memchecker_metadata*)malloc(sizeof(struct memchecker_metadata));
  
  list_initialize(&metadata->node);
  list_add_tail(&metadata_list, &metadata->node);
  
  metadata->state = MEMCHECKER_ALLOCATED;
  metadata->size = size;
  
  unsigned long addr = 
    (unsigned long)malloc(sizeof(struct memchecker_metadata)
    + size + 2 * sizeof(MEMCHECKER_CANARY));
    
  if(!addr)
    {
      syslog(LOG_ERR, "Alloc failed");
      return NULL;
    }
  
  metadata->addr = addr + sizeof(metadata) + MEMCHECKER_CANARY;
  for_each_canary(metadata, set_canary_byte);
  
  syslog(LOG_INFO, "Alloc succeed: %p", metadata->addr);
  return (void *)metadata->addr;
}

void memchecker_free(const char *file, int line, const void *addr)
{
  struct memchecker_metadata *metadata = 
    addr_to_metadata((unsigned long)addr);
  
  // printf("addr: %p\n", metadata);
  
  /* Double free */
  if(metadata->state == MEMCHECKER_FREED)
    {
      metadata->state = MEMCHECKER_ERROR;
      metadata->error_type = ERROR_DOUBLE_FREE;
    }
  
  for_each_canary(metadata, check_canary_byte);  
  
  if(metadata->state == MEMCHECKER_ALLOCATED)
    {
      metadata->state = MEMCHECKER_FREED;
      for_each_canary(metadata, set_canary_byte);                      
      list_add_tail(&metadata_freelist, &metadata->node);
    }
    
  if(metadata->state == MEMCHECKER_ERROR)
    {
      switch(metadata->error_type)
        {
          case ERROR_OUT_OF_MEMORY: syslog(LOG_ERR, "Error : OOM"); break;
          case ERROR_USE_AFTER_FREE: syslog(LOG_ERR, "Error : UAF"); break;
          case ERROR_DOUBLE_FREE: syslog(LOG_ERR, "Error : DF"); break;
        } 
      list_add_tail(&metadata_errorlist, &metadata->node);
    }
    
  /* Set timer */    
}

void memchecker_init(void)
{
  /* Metadata list initialize */
  list_initialize(&metadata_list);
  list_initialize(&metadata_freelist);
  list_initialize(&metadata_errorlist);
}

