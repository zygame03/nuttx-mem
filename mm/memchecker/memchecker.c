/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <sched.h>
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

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void error_report(struct memchecker_metadata *metadata)
{
  char *error_msg[] = {"Out of bounds",
                       "Use after free",
                       "Double free",
                       "Invalid free"};
  int pid = getpid();
  struct tcb_s *task = nxsched_get_tcb(pid);
  syslog(LOG_ERR, "=================================================");
  syslog(LOG_ERR, "BUG: MEMCHCHECKER: %s", error_msg[metadata->error_type]);
  syslog(LOG_ERR, "backtrace");
  syslog(LOG_ERR, "memchecker: Range: %p - %p", 
         metadata->addr - MEMCHECKER_CANARY, 
         metadata->addr + metadata->size + MEMCHECKER_CANARY);
  syslog(LOG_ERR, "            Size: %d", metadata->size);
  //system
  syslog(LOG_ERR, "CPU: 0, PID: %d, Taskname: %s", pid, task->name);

#ifdef CONFIG_VERSION_STRING
  syslog(LOG_ERR, "NuttX Version: %s", CONFIG_VERSION_STRING);
#endif

  syslog(LOG_ERR, "=================================================");
}

static struct memchecker_metadata *addr_to_metadata(unsigned long addr)
{
  struct memchecker_metadata *metadata;
  list_for_every_entry(&metadata_list, metadata,
                       struct memchecker_metadata, node)
    {
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
		      metadata->error_type = ERROR_OUT_OF_BOUDNDS;
          error_report(metadata);
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
              error_report(metadata);
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
	        metadata->error_type = ERROR_OUT_OF_BOUDNDS;
          error_report(metadata);
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
    (struct memchecker_metadata *)malloc(sizeof(struct memchecker_metadata));
  
  list_initialize(&metadata->node);
  list_add_tail(&metadata_list, &metadata->node);
  
  metadata->state = MEMCHECKER_ALLOCATED;
  metadata->size = size;
  
  unsigned long addr = 
    (unsigned long)malloc(size + 2 * MEMCHECKER_CANARY);
    
  if(!addr)
    {
      syslog(LOG_ERR, "Alloc failed");
      return NULL;
    }
  
  metadata->addr = addr + MEMCHECKER_CANARY;
  for_each_canary(metadata, set_canary_byte);
  
  // syslog(LOG_INFO, "Alloc succeed: %p", metadata->addr);
  return (void *)metadata->addr;
}

void memchecker_free(const char *file, int line, const void *addr)
{
  struct memchecker_metadata *metadata = 
    addr_to_metadata((unsigned long)addr);
  
  if(metadata->state == MEMCHECKER_ALLOCATED)
    {
      for_each_canary(metadata, check_canary_byte);  
      metadata->state = MEMCHECKER_FREED;
      for_each_canary(metadata, set_canary_byte); 
    }
  else
    {
      for_each_canary(metadata, check_canary_byte);
      metadata->state = MEMCHECKER_ERROR;
      metadata->error_type = ERROR_DOUBLE_FREE;
      error_report(metadata);
      return;
    }
  /* Set timer */    
}

void print_metadata_info()
{
  struct memchecker_metadata *metadata;
  int count = 1;
  printf("================list===============\n");
  list_for_every_entry(&metadata_list, metadata,
                       struct memchecker_metadata, node)
    {
      printf("metadata_%d\n", count);
      printf("size: %d, addr: %p\n", metadata->size, metadata->addr);
      printf("state: %d", metadata->state);
      if(metadata->state == MEMCHECKER_ERROR)
        {
          printf(", error: %d", metadata->error_type);
        }
      printf("\n");
      count++;
    }   
  printf("================list===============\n");
}

void memchecker_init(void)
{
  /* Metadata list initialize */
  list_initialize(&metadata_list);
}

