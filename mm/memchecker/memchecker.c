/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <nuttx/symtab.h>
#include <nuttx/arch.h>
#include <nuttx/atomic.h>
#include "leakdetector.h"
#include <nuttx/mm/memchecker.h>
#include <sched.h>
#include <syslog.h>
#include <execinfo.h>
#include <sys/time.h>
#include <time.h>
#include <nuttx/atomic.h>
#include <nuttx/syslog/syslog.h>
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

#define MEMCHECKER_TIMEOUT 10 * 100

/****************************************************************************
 * Global Data
 ****************************************************************************/

#ifdef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
struct task_stats_list_lock *hf;

struct task_stats_list_lock *lf;
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct memchecker_list_node
{
  struct list_node head;
  spinlock_t lock;
};

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
static struct memchecker_metadata metadata_list[MEMCHECKER_PAGE_NUMBER];

static char *memchecker_pool;

#ifdef CONFIG_MM_MEMCHECKER_ALLOC_TIMER
static struct work_s alloc_timer_work;

static bool can_alloc = true;
#endif

static struct memchecker_list_node usable_list;
#endif

static struct memchecker_list_node allocated_list;

static struct memchecker_list_node freed_list;

static struct memchecker_list_node error_list;

static const char *error_msg[] = {"no error",
                                  "Out of bounds",
                                  "Use after free",
                                  "Invalid free",
                                  "Memory leak"};

static struct work_s manager_work;

float decay_factor = 0.95;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t fast_active_hash(void *data, size_t size)
{
  const uint64_t *blocks = (const uint64_t *)data;
  size_t num_blocks = size / sizeof(uint64_t);
  uint32_t hash = 0x9e3779b9;

  for (size_t i = 0; i < num_blocks; i++)
  {
    hash ^= (blocks[i] & 0xFFFFFFFF);
    hash ^= ((blocks[i] >> 32) & 0xFFFFFFFF);
    hash = (hash << 13) | (hash >> 19);
  }

  const uint8_t *tail = (const uint8_t *)(blocks + num_blocks);
  uint64_t tail_value = 0;
  for (size_t i = 0; i < size % sizeof(uint64_t); i++)
  {
    tail_value |= ((uint64_t)tail[i] << (i * 8));
  }
  hash ^= (tail_value & 0xFFFFFFFF);
  hash ^= ((tail_value >> 32) & 0xFFFFFFFF);

  return hash;
}

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
#ifdef CONFIG_MM_MEMCHECKER_ALLOC_TIMER
static void memchecker_alloc_timer(FAR void *argv)
{
  can_alloc = true;
  return;
}
#endif

static struct memchecker_metadata *index_to_metadata(int index)
{
  return &metadata_list[index];
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
  if (!is_memchecker_addr(addr))
  {
    return NULL;
  }

  int index = (addr - (unsigned long)memchecker_pool) / MEMCHECKER_PAGE_SIZE;
  return index_to_metadata(index);
}

static unsigned long metadata_to_addr(struct memchecker_metadata *metadata)
{
  unsigned long offset = (metadata - metadata_list) * MEMCHECKER_PAGE_SIZE;
  return (unsigned long)memchecker_pool + offset;
}

static void memchecker_alloc_pool(void)
{
  memchecker_pool = (char *)malloc(MEMCHECKER_POOL_SIZE);
  if (!memchecker_pool)
  {
    syslog(LOG_ERR, "Memchecker pool init failed");
    return;
  }
  memset(memchecker_pool, 0, MEMCHECKER_POOL_SIZE);
}

static void memchecker_init_pool(void)
{
  memchecker_alloc_pool();
  irqstate_t flags;

  if (!memchecker_pool)
  {
    return;
  }

  for (int i = 0; i < MEMCHECKER_PAGE_NUMBER; i++)
  {
    struct memchecker_metadata *metadata = &metadata_list[i];

    spin_lock_init(&metadata->lock);

    spin_lock(&metadata->lock);
    metadata->state = MEMCHECKER_UNUSED;
    metadata->error_type = ERROR_NO_ERROR;
    metadata->activity_score = 50;
    metadata->pid = -1;

    list_initialize(&metadata->node);

    flags = spin_lock_irqsave(&usable_list.lock);
    list_add_tail(&usable_list.head, &metadata->node);
    spin_unlock_irqrestore(&usable_list.lock, flags);

    spin_unlock(&metadata->lock);
  }
}

static void memchecker_show_memory(uint8_t *start, uint8_t *addr)
{
  FAR const uint8_t *end = start + MEMCHECKER_PAGE_SIZE;
  FAR const uint8_t *p = start;
  char buffer[256];

  syslog(LOG_ERR, "Shadow bytes around the buggy address:\n");
  for (p = start; p < end; p += 16)
  {
    int ret = sprintf(buffer, "  %p: ", p);
    int i;
    for (i = 0; i < 16; i++)
    {
      if (p + i - start < MEMCHECKER_BOUND_SIZE ||
          p + i >= end - MEMCHECKER_BOUND_SIZE)
      {
        ret += sprintf(buffer + ret, "\033[31m%02x\033[0m ", p[i]);
      }
      else if (p + i == addr)
      {
        ret += sprintf(buffer + ret, "\b[\033[37m%02x\033[0m ", p[i]);
      }
      else if (p + i == end - MEMCHECKER_BOUND_SIZE - 1)
      {
        ret += sprintf(buffer + ret, "\033[37m%02x\033[0m]", p[i]);
      }
      else
      {
        ret += sprintf(buffer + ret, "\033[37m%02x\033[0m ", p[i]);
      }
    }
    syslog(LOG_ERR, "%s", buffer);
  }
}

static void memchecker_print_stack(struct memchecker_metadata *metadata)
{
  struct symtab_s *symbol;
  size_t size;

  struct tcb_s *tcb = nxsched_get_tcb(metadata->pid);

#ifdef CONFIG_SMP
  uint8_t cpu = tcb->cpu;
#else
  uint8_t cpu = 0;
#endif

  syslog(LOG_ERR, "alloc_track: task: %s", tcb->name);
  syslog(LOG_ERR, "pid: %d cpu: %d file: %s line: %d",
         metadata->pid, cpu, metadata->alloc_track.file,
         metadata->alloc_track.line);

  for (int i = 0; i < metadata->alloc_track.num_stack_entries; i++)
  {
    symbol = (struct symtab_s *)
        allsyms_findbyvalue((void *)metadata->alloc_track.stack_entries[i],
                            &size);
    if (symbol != NULL)
    {
      syslog(LOG_ERR, "[%2d] [%p] %-16s", i,
             (void *)metadata->alloc_track.stack_entries[i],
             symbol->sym_name);
    }
    else
    {
      syslog(LOG_ERR, "[%d] %p", i,
             (void *)metadata->alloc_track.stack_entries[i]);
    }
  }

  if (metadata->state == MEMCHECKER_ALLOCATED)
  {
    return;
  }
}

#else /* CONFIG_MM_MEMCHECKER_LEAKDETECTOR */
// 遍历四个链表找到对应的metadata
static struct memchecker_metadata *addr_to_metadata(unsigned long addr)
{
  struct memchecker_metadata *metadata = NULL;
  spin_lock(&allocated_list.lock);
  list_for_every_entry(&allocated_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->addr == addr)
    {
      return metadata;
    }
  }
  spin_unlock(&allocated_list.lock);
  spin_lock(&freed_list.lock);
  list_for_every_entry(&freed_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->addr == addr)
    {
      return metadata;
    }
  }
  spin_unlock(&freed_list.lock);
  spin_lock(&error_list.lock);
  list_for_every_entry(&error_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->addr == addr)
    {
      return metadata;
    }
  }
  spin_unlock(&error_list.lock);
  return NULL;
}

static void memchecker_show_memory(uint8_t *start, size_t size)
{
  FAR const uint8_t *end = start + 2 * MEMCHECKER_BOUND_SIZE + size;
  FAR const uint8_t *p = start;
  char buffer[256];

  syslog(LOG_ERR, "Shadow bytes around the buggy address:\n");
  for (p = start; p < end; p += 16)
  {
    int ret = sprintf(buffer, "  %p: ", p);
    int i;
    for (i = 0; i < 16; i++)
    {
      if (p + i - start < MEMCHECKER_BOUND_SIZE ||
          p + i >= end - MEMCHECKER_BOUND_SIZE)
      {
        ret += sprintf(buffer + ret, "\033[31m%02x\033[0m ", p[i]);
      }
      else if (p + i == start + MEMCHECKER_BOUND_SIZE)
      {
        ret += sprintf(buffer + ret, "\b[\033[37m%02x\033[0m ", p[i]);
      }
      else if (p + i == end - MEMCHECKER_BOUND_SIZE - 1)
      {
        ret += sprintf(buffer + ret, "\033[37m%02x\033[0m]", p[i]);
      }
      else
      {
        ret += sprintf(buffer + ret, "\033[37m%02x\033[0m ", p[i]);
      }
    }
    syslog(LOG_ERR, "%s", buffer);
  }
}

static unsigned long metadata_to_addr(struct memchecker_metadata *metadata)
{
  return metadata->addr - MEMCHECKER_BOUND_SIZE;
}

#endif /* CONFIG_MM_MEMCHECKER_LEAKDETECTOR */

static void memchecker_report(struct memchecker_metadata *metadata)
{
  syslog(LOG_ERR, "====================MEMCHECKER========================");
  syslog(LOG_ERR, "BUG: %s", error_msg[metadata->error_type]);

  syslog(LOG_ERR, "Address: %p - %p",
         (void *)metadata->addr, (void *)metadata->addr + metadata->size);

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  memchecker_show_memory((uint8_t *)metadata_to_addr(metadata),
                         (uint8_t *)metadata->addr);
#else
  memchecker_show_memory((uint8_t *)metadata->addr, metadata->size);
#endif

  dump_stack();

  syslog(LOG_ERR, "======================================================");
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
  for (addr = metadata_to_addr(metadata); addr < metadata->addr; addr++)
  {
    if (!fn((uint8_t *)addr))
    {
      spin_lock(&metadata->lock);

      if (metadata->state == MEMCHECKER_FREED)
      {
        metadata->error_type = ERROR_USE_AFTER_FREE;

        memchecker_report(metadata);
      }

      metadata->state = MEMCHECKER_ERROR;
      metadata->error_type = ERROR_OUT_OF_BOUDNDS;
      memchecker_report(metadata);
      spin_unlock(&metadata->lock);

      break;
    }
  }

  if (metadata->state != MEMCHECKER_ALLOCATED)
  {
    for (addr = metadata->addr; addr < metadata->addr + metadata->size;
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
       addr < metadata->addr + metadata->size + MEMCHECKER_BOUND_SIZE; addr++)
  {
    if (!fn((uint8_t *)addr))
    {
      if (metadata->state == MEMCHECKER_FREED)
      {
        metadata->error_type = ERROR_USE_AFTER_FREE;
        memchecker_report(metadata);
      }

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
  irqstate_t flags;

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  if (!list_is_empty(&usable_list.head))
  {
    flags = spin_lock_irqsave(&usable_list.lock);
    metadata = list_entry(usable_list.head.next,
                          struct memchecker_metadata, node);
    list_delete(&metadata->node);
    spin_unlock_irqrestore(&usable_list.lock, flags);

    flags = spin_lock_irqsave(&allocated_list.lock);
    list_add_tail(&allocated_list.head, &metadata->node);
    spin_unlock_irqrestore(&allocated_list.lock, flags);
  }

  if (!metadata)
  {
    return malloc(size);
  }

  spin_lock(&metadata->lock);

#ifdef CONFIG_MM_MEMCHECKER_ALLOC_TIMER
  can_alloc = false;
  work_queue(HPWORK, &alloc_timer_work, (worker_t)memchecker_alloc_timer,
             NULL, SEC2TICK(CONFIG_MM_MEMCHECKER_ALLOC_TIME));
#endif

  unsigned long addr = metadata_to_addr(metadata) + MEMCHECKER_BOUND_SIZE +
                       MEMCHECKER_DATA_SIZE - size;

#else /* CONFIG_MM_MEMCHECKER_LEAKDETECTOR */
  metadata = (struct memchecker_metadata *)malloc(sizeof(struct memchecker_metadata));

  if (!metadata)
  {
    syslog(LOG_ERR, "memchecker_guarded_alloc: malloc failed");
    return malloc(size);
  }

  spin_lock(&metadata->lock);

  list_initialize(&metadata->node);

  flags = spin_lock_irqsave(&allocated_list.lock);
  list_add_tail(&allocated_list.head, &metadata->node);
  spin_unlock_irqrestore(&allocated_list.lock, flags);

  unsigned long addr = (unsigned long)malloc(size + 2 * MEMCHECKER_BOUND_SIZE);
  if (!addr)
  {
    syslog(LOG_ERR, "memchecker_guarded_alloc: malloc failed");
    free(metadata);
    return malloc(size);
  }
#endif

  metadata->addr = addr;
  metadata->size = size;
  metadata->state = MEMCHECKER_ALLOCATED;
  metadata->pid = getpid();

  /* 记录分配信息 */
  metadata->alloc_track.ts = clock_systime_ticks();
  strcpy(metadata->alloc_track.file, file);
  metadata->alloc_track.line = line;
  metadata->alloc_track.num_stack_entries =
      sched_backtrace(metadata->pid,
                      (void **)metadata->alloc_track.stack_entries, 32, 0);

#ifdef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  int i;
  /*** 一开始默认都是添加到高频扫描链表中 */
  /*** 如果不为空, 则默认   */
  WARN();
  i = add_metadata_to_task_mem_stats(metadata);
  if (i)
  {
    syslog(LOG_WARNING, "%s failed to add metadata...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  }

#endif
  spin_unlock(&metadata->lock);

  for_each_canary(metadata, set_canary_byte);
  for_each_canary(metadata, set_canary_byte);
  return (void *)addr;
}

static void memchecker_guarded_free(const char *file, int line, void *addr)
{
  struct memchecker_metadata *metadata =
      addr_to_metadata((unsigned long)addr);

  irqstate_t flags;
  spin_lock(&metadata->lock);

  if (metadata->addr != (unsigned long)addr)
  {
    metadata->state = MEMCHECKER_ERROR;
    metadata->error_type = ERROR_INVALID_FREE;
    memchecker_report(metadata);
    spin_unlock(&metadata->lock);
    return;
  }

  if (metadata->state == MEMCHECKER_ALLOCATED)
  {
    for_each_canary(metadata, check_canary_byte);
    metadata->state = MEMCHECKER_FREED;
    list_delete_init(&metadata->node);

    flags = spin_lock_irqsave(&freed_list.lock);
    list_add_tail(&freed_list.head, &metadata->node);
    spin_unlock_irqrestore(&freed_list.lock, flags);

    for_each_canary(metadata, set_canary_byte);
  }
  else
  {
    for_each_canary(metadata, check_canary_byte);

    if (metadata->state == MEMCHECKER_FREED)
    {
      metadata->state = MEMCHECKER_ERROR;
      list_delete_init(&metadata->node);

      flags = spin_lock_irqsave(&error_list.lock);
      list_add_tail(&error_list.head, &metadata->node);
      spin_unlock_irqrestore(&error_list.lock, flags);
    }

    metadata->error_type = ERROR_INVALID_FREE;
    memchecker_report(metadata);
  }

  metadata->free_track.ts = clock_systime_ticks();
  strcpy(metadata->free_track.file, file);
  metadata->free_track.line = line;
  metadata->free_track.num_stack_entries =
      up_backtrace(nxsched_get_tcb(metadata->pid),
                   (void **)metadata->free_track.stack_entries, 32, 0);
#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  WARN();
  int i;
  i = update_task_mem_stats_when_free(metadata);
  if (i)
  {
    syslog(LOG_INFO, "%sserious problem, updating failed...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  }
#endif
  spin_unlock(&metadata->lock);
}

static void update_activity(struct memchecker_metadata *metadata)
{
  uint32_t current_hash =
      fast_active_hash((void *)metadata->addr, metadata->size);

  int change_intensity = __builtin_popcount(metadata->last_hash ^ current_hash);

  if (current_hash != metadata->last_hash)
  {
    metadata->activity_score += change_intensity * 2;
    metadata->activity_score =
        (metadata->activity_score > 100) ? 100 : metadata->activity_score;
  }
  else
  {
    metadata->activity_score *= decay_factor;
  }

  metadata->last_hash = current_hash;
  // syslog(LOG_INFO, "activity_score: %d", metadata->activity_score);
}

static void metadata_update_activity(void)
{
  struct memchecker_metadata *metadata;
  struct list_node *node;
  irqstate_t flags;

  for (node = allocated_list.head.next; node != &allocated_list.head; node = node->next)
  {
    metadata = list_entry(node, struct memchecker_metadata, node);
    spin_lock(&metadata->lock);
    update_activity(metadata);

    if (metadata->activity_score < 10)
    {
      syslog(LOG_WARNING, "Memory leak detected: %p - %p",
             (void *)metadata->addr, (void *)metadata->addr + metadata->size);
      metadata->error_type = ERROR_MEMORY_LEAK;

      node = node->prev;
      list_delete_init(&metadata->node);

      flags = spin_lock_irqsave(&error_list.lock);
      list_add_tail(&error_list.head, &metadata->node);
      spin_unlock_irqrestore(&error_list.lock, flags);

      memchecker_report(metadata);
    }

    spin_unlock(&metadata->lock);
  }
}

static void metadata_timeout(void)
{
  time_t now_ts = clock_systime_ticks();
  struct memchecker_metadata *metadata;
  struct list_node *node;
  irqstate_t freed_list_flags;
  irqstate_t usable_list_flags;

  freed_list_flags = spin_lock_irqsave(&freed_list.lock);
  for (node = freed_list.head.next; node != &freed_list.head; node = node->next)
  {
    metadata = list_entry(node, struct memchecker_metadata, node);
    if (now_ts - metadata->free_track.ts > MEMCHECKER_TIMEOUT)
    {
      node = node->prev;
      list_delete_init(&metadata->node);

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
      usable_list_flags = spin_lock_irqsave(&usable_list.lock);
      list_add_tail(&usable_list.head, &metadata->node);
      spin_unlock_irqrestore(&usable_list.lock, usable_list_flags);
#else
      list_delete(&metadata->node_for_ld);
      free((void *)metadata->addr);
      free(metadata);
#endif
    }
  }
  spin_unlock_irqrestore(&freed_list.lock, freed_list_flags);
}

static void metadata_error_operation(void)
{
  // time_t now_ts = clock_systime_ticks();

  struct memchecker_metadata *metadata;
  struct list_node *node;

  for (node = error_list.head.next; node != &error_list.head; node = node->next)
  {
    metadata = list_entry(node, struct memchecker_metadata, node);
    switch (metadata->error_type)
    {
    case ERROR_OUT_OF_BOUDNDS:
      break;
    case ERROR_USE_AFTER_FREE:
      break;
    case ERROR_INVALID_FREE:
      break;
    default:
      break;
    }
  }
  return;
}

static void metadata_manager(FAR void *arg)
{

  metadata_timeout();
  // metadata_error_operation();
  metadata_update_activity();

  work_queue(HPWORK, &manager_work,
             (worker_t)metadata_manager, NULL, SEC2TICK(1));
  return;
}

static void init_metadata_manager(void)
{
  work_queue(HPWORK, &manager_work,
             (worker_t)metadata_manager, NULL, SEC2TICK(1));
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

#ifdef CONFIG_MM_MEMCHECKER_ALLOC_TIMER
  if (!can_alloc)
  {
    return malloc(size);
  }
#endif

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
  // 这个函数还没有在开启ld时的实现
  // if (!is_memchecker_addr((unsigned long)addr))
  // {
  //   free((void *)addr);
  //   return;
  // }

  return memchecker_guarded_free(file, line, (void *)addr);
}

// 在三个链表下搜索对应pid的metadata，返回个数，
int pid_to_metadata(pid_t pid, struct memchecker_metadata **metadata_list)
{
  int count = 0;
  struct memchecker_metadata *metadata;
  struct list_node *node;
  irqstate_t flags;

  flags = spin_lock_irqsave(&allocated_list.lock);
  list_for_every_entry(&allocated_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->pid == pid)
    {
      metadata_list[count] = metadata;
      count++;
    }
  }
  spin_unlock_irqrestore(&allocated_list.lock, flags);

  flags = spin_lock_irqsave(&freed_list.lock);
  list_for_every_entry(&freed_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->pid == pid)
    {
      metadata_list[count] = metadata;
      count++;
    }
  }
  spin_unlock_irqrestore(&freed_list.lock, flags);

  flags = spin_lock_irqsave(&error_list.lock);
  list_for_every_entry(&error_list.head, metadata, struct memchecker_metadata, node)
  {
    if (metadata->pid == pid)
    {
      metadata_list[count] = metadata;
      count++;
    }
  }
  spin_unlock_irqrestore(&error_list.lock, flags);

  return count;
}

void memchecker_init(void)
{
  // syslog_file_channel("/log/memchecker");

  list_initialize(&allocated_list.head);
  list_initialize(&freed_list.head);
  list_initialize(&error_list.head);
  spin_lock_init(&allocated_list.lock);
  spin_lock_init(&freed_list.lock);
  spin_lock_init(&error_list.lock);

#ifndef CONFIG_MM_MEMCHECKER_LEAKDETECTOR
  list_initialize(&usable_list.head);
  spin_lock_init(&usable_list.lock);
  memchecker_init_pool();
#else
  /**  通过函数导出对应的链表值 */
  get_task_list_lock_hf(&hf);
  get_task_list_lock_lf(&lf);

  /*** 高频  */
  spin_lock_init(&(hf->tms_list_lock));
  list_initialize(&(hf->task_mem_status_list));

  /**  低频 */
  spin_lock_init(&(lf->tms_list_lock));
  list_initialize(&(lf->task_mem_status_list));
#endif /* CONFIG_MM_MEMCHECKER_LEAKDETECTOR */

  init_metadata_manager();
}
