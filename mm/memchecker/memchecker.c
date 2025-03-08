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
#include <nuttx/clock.h>
#include <time.h>
#include "mmdebug.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef malloc
#undef free

#define MEMCHECKER_PAGE_NUMBER CONFIG_MM_MEMCHECKER_PAGE_NUMBER
#define MEMCHECKER_BOUND_SIZE CONFIG_MM_MEMCHECKER_BOUND_SIZE
#define MEMCHECKER_DATA_SIZE CONFIG_MM_MEMCHECKER_DATA_SIZE

#define MEMCHECKER_PAGE_SIZE \
  (MEMCHECKER_DATA_SIZE + 2 * MEMCHECKER_BOUND_SIZE)
#define MEMCHECKER_POOL_SIZE (MEMCHECKER_PAGE_NUMBER * MEMCHECKER_PAGE_SIZE)

#define MEMCHECKER_TIMEOUT 360000 

/****************************************************************************
 * Global Variables
 ****************************************************************************/

#ifdef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
extern struct list_node task_mem_status_list;
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

static struct memchecker_metadata metadata_list[MEMCHECKER_PAGE_NUMBER];

static struct list_node usable_list;

static struct list_node allocated_list;

static struct list_node freed_list;

static struct list_node error_list;

static char *memchecker_pool;

static const char *error_msg[] = {"no error",
                                  "Out of bounds",
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

time_t get_timestamp(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void memchecker_report(struct memchecker_metadata *metadata)
{
  int pid = getpid();
  struct tcb_s *task = nxsched_get_tcb(pid);

  syslog(LOG_ERR, "====================MEMCHECKER========================");
  syslog(LOG_ERR, "BUG: %s", error_msg[metadata->error_type]);

  syslog(LOG_ERR, "Address: %p - %p", 
    (void *)metadata->addr - MEMCHECKER_BOUND_SIZE, 
    (void *)metadata->addr + metadata->size + MEMCHECKER_BOUND_SIZE);
  //system
  syslog(LOG_ERR, "CPU: 0, PID: %d, Taskname: %s", pid, task->name);
  
  struct tm *time_info = localtime(&metadata->alloc_ts);
  char buffer[80];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", time_info);
  printf("Allocation time: %s\n", buffer);
  
  time_info = localtime(&metadata->free_ts);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", time_info);
  printf("Free time: %s\n", buffer);
  
  syslog(LOG_ERR, "tim: %lu", metadata->alloc_ts);

  // print_metadata(metadata);
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
      list_add_tail(&usable_list, &metadata->node);
      metadata->addr = (unsigned long)memchecker_pool + 
        i * MEMCHECKER_PAGE_SIZE + MEMCHECKER_BOUND_SIZE;
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

  if(!list_is_empty(&usable_list))
    {
      metadata = list_entry(usable_list.next, struct memchecker_metadata, node);
      list_delete(&metadata->node);
      list_add_tail(&allocated_list, &metadata->node);
    }
    
  if(!metadata)
    {
      return malloc(size);
    }
  
  metadata->alloc_ts = clock_systime_ticks();
  metadata->size = size;
  metadata->state = MEMCHECKER_ALLOCATED;
  for_each_canary(metadata, set_canary_byte);

  metadata->pid = getpid();
  strcpy(metadata->file, file);
  metadata->line = line;
  metadata->stack_depth = 
  up_backtrace(nxsched_get_tcb(metadata->pid), 
               (void **)metadata->stack, 32, 0);

  return (void *)metadata->addr;
}

static void memchecker_guarded_free(const char *file, int line, void *addr)
{
  struct memchecker_metadata *metadata = 
  addr_to_metadata((unsigned long)addr);
  
  if(!metadata)
    {
      free((void *)addr);
      return;
    }

  metadata->free_ts = clock_systime_ticks();
  for_each_canary(metadata, check_canary_byte);  

  if(metadata->state == MEMCHECKER_ALLOCATED)
    {
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
      metadata->state = MEMCHECKER_ERROR;
      metadata->error_type = ERROR_DOUBLE_FREE;
      memchecker_report(metadata);
    } 
}

static void metadata_manager(void)
{
  return;
}

static void metadata_timeout(void)
{
  time_t now_ts = get_timestamp();
  struct memchecker_metadata *metadata;
  struct list_node *node;

  for(node = freed_list.next; node != &freed_list; node = node->next)
    {
      metadata = list_entry(node, struct memchecker_metadata, node);
      if(now_ts - metadata->free_ts > MEMCHECKER_TIMEOUT) 
        {
          list_delete_init(&metadata->node);
          list_add_tail(&usable_list, &metadata->node);
        }
    }
}

static void metadata_error_operation(void)
{
  return;
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

  return memchecker_guarded_free(file, line, (void *)addr);
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

/****************************************************************************
 *  Name: pid_to_metadata
 * 
 *  Description:
 *    Get the metadata with pid.
 *    *from all metadata
 * 
 *  Input Parameters:
 *    pid - The pid of the task.
 *    metadata - The buffer to store the metadata.
 * 
 *  Returned Value:
 *    The number of metadata.
 *    return 0 if no metadata found.
 ****************************************************************************/
// 待优化
int pid_to_metadata(pid_t pid, struct memchecker_metadata *buffer[])
{
  int count = 0;
  for(int i = 0; i < MEMCHECKER_PAGE_NUMBER; i++)
    {
      if(metadata_list[i].pid == pid)
        {
          buffer[count++] = &metadata_list[i];
        }
    }
  return count;
}

void memchecker_init(void)
{
  list_initialize(&usable_list);
  list_initialize(&allocated_list);
  list_initialize(&freed_list);
  list_initialize(&error_list);
  memchecker_init_pool();

#ifdef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  list_initialize(&task_mem_status_list);
  init_leak_detection();
#endif  
}
