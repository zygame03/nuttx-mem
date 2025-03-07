/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <syslog.h>
#include <execinfo.h>
#include <nuttx/allsyms.h>
#include <nuttx/symtab.h>
#include <nuttx/arch.h>
#include <sys/time.h>
#include <time.h>
#include "hook.h"

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
 * Global Variables
 ****************************************************************************/

//  extern const struct symtab_s g_allsyms[];
//  extern const int             g_nallsyms;

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Memory checker metadata list*/

static struct memchecker_metadata metadata_list[MEMCHECKER_PAGE_NUMBER];

/* can malloc metadata list */

static struct list_node free_list;

/* error metadata list */

static struct list_node error_list;

/* the ptr to the memory pool */

static char *memchecker_pool;

/* error type to string*/

static const char *error_msg[] = {"Out of bounds",
                                  "Use after free",
                                  "Double free",
                                  "Invalid free",
                                  "Memory leak"};

// static struct work_s g_memcheck_work;  

// static int process_list[MAX_TASKS];

static void memchecker_report(struct memchecker_metadata *metadata);

static bool set_canary_byte(uint8_t *addr);

static bool check_canary_byte(uint8_t *addr);

static void for_each_canary(struct memchecker_metadata *metadata, 
                            bool (*fn)(uint8_t *));

void print_metadata(struct memchecker_metadata *metadata);

time_t get_timestamp(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void memchecker_report(struct memchecker_metadata *metadata)
{
  int pid = getpid();
  struct tcb_s *task = nxsched_get_tcb(pid);

  syslog(LOG_ERR, "======================================================");
  syslog(LOG_ERR, "BUG: MEMCHCHECKER: %s", error_msg[metadata->error_type]);

  syslog(LOG_ERR, "Address: %p - %p", 
    (void *)metadata->addr - MEMCHECKER_BOUND_SIZE, 
    (void *)metadata->addr + metadata->size + MEMCHECKER_BOUND_SIZE);
  //system
  syslog(LOG_ERR, "CPU: 0, PID: %d, Taskname: %s", pid, task->name);

  print_metadata(metadata);
  syslog(LOG_ERR, "Backtrace:");

  struct symtab_s *symbol;
  size_t size;

  for(int i = 0; i < metadata->stack_depth; i++)
  {
    symbol = (struct symtab_s *)allsyms_findbyvalue((void *)metadata->stack[i], &size);
    if (symbol != NULL)
      {
        syslog(LOG_ERR, "[%2d] [%p] %-16s", i, 
              (void *)metadata->stack[i], symbol->sym_name);
      }
    else
      {
        syslog(LOG_ERR, "[%d] %p", i, (void *)metadata->stack[i]);
      } 
    // sleep(1);
  }

  
#ifdef CONFIG_VERSION_STRING
  syslog(LOG_ERR, "NuttX Version: %s", CONFIG_VERSION_STRING);
#endif

  syslog(LOG_ERR, "======================================================");
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

static void memchecker_alloc_pool(void)
{
  memchecker_pool = (char *)malloc(MEMCHECKER_POOL_SIZE);
  if(!memchecker_pool)
    {
      syslog(LOG_ERR, "Memchecker pool init failed");
      return; 
    }
  memset(memchecker_pool, 0, MEMCHECKER_POOL_SIZE);
}

static void memchecker_init_pool(void)
{
  memchecker_alloc_pool();
  if(!memchecker_pool)
    {
      return;
    }
  
  for(int i = 0; i < MEMCHECKER_PAGE_NUMBER; i++)
    {
      struct memchecker_metadata *metadata = &metadata_list[i];
      list_initialize(&metadata->node);
      list_add_tail(&free_list, &metadata->node);
      metadata->addr = (unsigned long)memchecker_pool + 
        i * MEMCHECKER_PAGE_SIZE + MEMCHECKER_BOUND_SIZE;
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
          memchecker_report(metadata);
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
              memchecker_report(metadata);
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
          memchecker_report(metadata);
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
      return malloc(size);
    }
  
  metadata->size = size;
  metadata->state = MEMCHECKER_ALLOCATED;
  for_each_canary(metadata, set_canary_byte);

  metadata->pid = getpid();
  strcpy(metadata->file, file);
  metadata->line = line;
  metadata->ts = get_timestamp();
  metadata->stack_depth = 
  up_backtrace(nxsched_get_tcb(metadata->pid), 
               (void **)metadata->stack, 32, 0);

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
  if(!is_memchecker_addr((unsigned long)addr))
    {
      free((void *)addr);
      return;
    }

  struct memchecker_metadata *metadata = 
    addr_to_metadata((unsigned long)addr);
  
  if(!metadata)
    {
      free((void *)addr);
      return;
    }
  
  if(metadata->state == MEMCHECKER_ALLOCATED)
    {
      for_each_canary(metadata, check_canary_byte);  
      metadata->state = MEMCHECKER_FREED;
      for_each_canary(metadata, set_canary_byte); 
    }
  else
    {
      if(metadata->state != MEMCHECKER_ERROR)
      {
        list_delete_init(&metadata->node);
        list_add_tail(&error_list, &metadata->node);
      }
      for_each_canary(metadata, check_canary_byte);
      metadata->state = MEMCHECKER_ERROR;
      metadata->error_type = ERROR_DOUBLE_FREE;
      memchecker_report(metadata);
      return;
    }
  /* Set timer */    
}

struct memchecker_metadata *index_to_metadata(int index)
{
  return &metadata_list[index];
}

struct memchecker_metadata *addr_to_metadata(unsigned long addr)
{
  if(!is_memchecker_addr(addr))
    {
      return NULL;
    }

  int index = (addr - (unsigned long)memchecker_pool) / MEMCHECKER_PAGE_SIZE;
  return index_to_metadata(index);
}

time_t get_timestamp(void)
{
  struct timespec ts;
  uint32_t us;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
  return us;
}

void format_timestamp(time_t timestamp) {
  struct tm *timeinfo;
  char buffer[80];

  timeinfo = localtime(&timestamp);

  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  syslog(LOG_INFO, "%s", buffer);
}

void print_metadata(struct memchecker_metadata *metadata)
{
  format_timestamp(metadata->ts);
}

void memchecker_init(void)
{
  /* Metadata list initialize */
  list_initialize(&free_list);
  list_initialize(&error_list);

  memchecker_init_pool();
}

