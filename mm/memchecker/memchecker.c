/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <syslog.h>
#include <execinfo.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef malloc
#undef free

#define MEMCHECKER_PAGE_NUMBER CONFIG_MM_MEMCHECKER_PAGE_NUMBER
#define MEMCHECKER_BOUND_SIZE CONFIG_MM_MEMCHECKER_BOUND_SIZE
#define MEMCHECKER_DATA_SIZE CONFIG_MM_MEMCHECKER_DATA_SIZE

#define MEMCHECKER_PAGE_SIZE (MEMCHECKER_DATA_SIZE + 2 * MEMCHECKER_BOUND_SIZE)
#define MEMCHECKER_POOL_SIZE (MEMCHECKER_PAGE_NUMBER * MEMCHECKER_PAGE_SIZE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

static struct memchecker_metadata metadata_list[MEMCHECKER_PAGE_NUMBER];

static struct list_node free_list;

static struct list_node error_list;

static char *memchecker_pool;

static const char *error_msg[] = {"Out of bounds",
                                  "Use after free",
                                  "Double free",
                                  "Invalid free",
                                  "Memory leak"};

static struct work_s g_memcheck_work;  

static int process_list[MAX_TASKS];

static void error_report(struct memchecker_metadata *metadata);

static bool set_canary_byte(uint8_t *addr);

static bool check_canary_byte(uint8_t *addr);

static void for_each_canary(struct memchecker_metadata *metadata, 
                            bool (*fn)(uint8_t *));

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void error_report(struct memchecker_metadata *metadata)
{
  int pid = getpid();
  struct tcb_s *task = nxsched_get_tcb(pid);
  syslog(LOG_ERR, "=================================================");
  syslog(LOG_ERR, "BUG: MEMCHCHECKER: %s", error_msg[metadata->error_type]);
  syslog(LOG_ERR, "Backtrace:");
  for(int i = 0; i < metadata->stack_depth; i++)
    {
      syslog(LOG_ERR, "[%d] %p\n", i, metadata->stack[i]);
    }
  syslog(LOG_ERR, "memchecker: Range: %p - %p", 
         (void *)metadata->addr - MEMCHECKER_BOUND_SIZE, 
         (void *)metadata->addr + metadata->size + MEMCHECKER_BOUND_SIZE);
  syslog(LOG_ERR, "            Size: %d", (int)metadata->size);
  //system
  syslog(LOG_ERR, "CPU: 0, PID: %d, Taskname: %s", pid, task->name);

#ifdef CONFIG_VERSION_STRING
  syslog(LOG_ERR, "NuttX Version: %s", CONFIG_VERSION_STRING);
#endif

  syslog(LOG_ERR, "=================================================");
}

static bool is_memchecker_addr(unsigned long addr)
{
  if (addr >= (unsigned long)memchecker_pool &&
      addr < (unsigned long)memchecker_pool + MEMCHECKER_POOL_SIZE)
    {
      return true;
    }
  return false;
}

static struct memchecker_metadata *addr_to_metadata(unsigned long addr)
{

  if(!is_memchecker_addr(addr))
    {
      return NULL;
    }
  struct memchecker_metadata *metadata;
  int index = (addr - (unsigned long)memchecker_pool) / MEMCHECKER_PAGE_SIZE;
  metadata = &metadata_list[index];
  return metadata;
}

static void memchecker_alloc_pool()
{
  memchecker_pool = (char *)malloc(MEMCHECKER_POOL_SIZE);
  if(!memchecker_pool)
    {
      syslog(LOG_ERR, "Memchecker pool init failed");
      return; 
    }
  memset(memchecker_pool, 0, MEMCHECKER_POOL_SIZE);
}

static void memchecker_init_pool()
{
  memchecker_alloc_pool();
  if(!memchecker_pool)
    {
      return;
    }
  syslog(LOG_INFO, "Memchecker pool init succeed, addr: %p", memchecker_pool);
  for(int i = 0; i < MEMCHECKER_PAGE_NUMBER; i++)
    {
      struct memchecker_metadata *metadata = &metadata_list[i];
      list_initialize(&metadata->node);
      list_add_tail(&free_list, &metadata->node);
      metadata->addr = (unsigned long)memchecker_pool + 
        i * MEMCHECKER_PAGE_SIZE + MEMCHECKER_BOUND_SIZE;
      syslog(LOG_INFO,"addr: %p\n", metadata->addr);
      metadata->size = 0;
     
      metadata->state = MEMCHECKER_UNUSED;
       /*时间戳*/
      metadata->error_type = ERROR_NONE;
      metadata->ts = 0;
      metadata->pid = 0;
      metadata->file[0] = '\0';
      metadata->line = 0;
    }
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
	for (addr = metadata->addr - MEMCHECKER_BOUND_SIZE;
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
	     addr < metadata->addr + MEMCHECKER_DATA_SIZE + MEMCHECKER_BOUND_SIZE;
       addr++)
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

static void *memchecker_guarded_alloc(const char *file, int line, size_t size)
{
  struct memchecker_metadata *metadata = NULL;

  if(!list_is_empty(&free_list))
    {
      metadata = list_entry(free_list.next, struct memchecker_metadata, node);
      list_delete_init(&metadata->node);
    }
  
  if(!metadata)
    {
      syslog(LOG_ERR, "Memchecker out of memory");
      return malloc(size);
    }
  
  metadata->size = size;
  metadata->state = MEMCHECKER_ALLOCATED;
  for_each_canary(metadata, set_canary_byte);

  metadata->pid = getpid();
  strcpy(metadata->file, file);
  metadata->line = line;
  metadata->ts = clock();
  metadata->stack_depth = 
    backtrace((void **)metadata->stack, MEMCHECKER_STACK_DEPTH);

  // syslog(LOG_INFO, "Alloc succeed: %p", metadata->addr);
  return (void *)metadata->addr;
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
  // 检查申请的内存大小
  if (size > MEMCHECKER_DATA_SIZE)
    {
      return malloc(size);
    }
    
  return memchecker_guarded_alloc(file, line, size);
}

/****************************************************************************
 * Name: memchecker_free
 * 
 * Description:
 *   Free memory with memory checker.
 * 
 * Input Parameters:
 *   file - The file name of the caller.
 *   line - The line number of the caller.
 *   addr - The address of the memory to be freed.
 * 
 * Returned Value:
 *   None.
 ****************************************************************************/

void memchecker_free(const char *file, int line, const void *addr)
{
  struct memchecker_metadata *metadata = 
    addr_to_metadata((unsigned long)addr);
  
  if(!metadata)
    {
      free(addr);
    }
  
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
  printf("================list===============\n");
  for(int i = 0; i < MEMCHECKER_PAGE_NUMBER; i++)
    {
      struct memchecker_metadata *metadata = &metadata_list[i];
      printf("metadata_%d\n", i);
      printf("size: %d, addr: %p\n", metadata->size, metadata->addr);
      printf("state: %d", metadata->state);
      if(metadata->state == MEMCHECKER_ERROR)
        {
          printf(", error: %d", metadata->error_type);
        }
      printf("\n");
    }   
  printf("================list===============\n");
}

void memchecker_init(void)
{
  /* Metadata list initialize */
  list_initialize(&free_list);
  list_initialize(&error_list);

  memchecker_init_pool();
}

