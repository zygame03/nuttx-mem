/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <nuttx/spinlock.h>
#include <syslog.h>
#include <nuttx/mutex.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/lib/math.h>
#include "leakdetector.h"
#include "utils.h"
#include "mmdebug.h"
#include "calcm.h"

/****************************************************************************
 * 用于调试输出
 ****************************************************************************/
#define LOG_FMT "%-16s: %-30u\n"
#define LOG_STR_FMT "%-16s: %-30s\n"
#define LOG_SIZE_FMT "%-16s: %-30lu\n"
#define SEPARATOR "========================================"

/****************************************************************************
 *  此处取消钩子函数, 为避免嵌套造成问题，此文件中的内存申请操作不计入统计
 ****************************************************************************/
#undef malloc
#undef free

/****************************************************************************
 *  g_leak_detection_work 泄漏检查工作任务
 *  task_mem_status_list  检查链表
 ****************************************************************************/
extern struct memchecker_metadata metadata_list[];

static struct work_s g_leak_detection_work;

/** 扫描频率较高 */
struct list_node task_mem_status_list;

/**  扫描频繁较低  low_frequency*/
struct list_node lowf_task_mem_status_list;

spinlock_t tms_list_lock;
/****************************************************************************
 * Name:Is_task_mem_stats_valid(pid_t pid)
 * Description:
 *   Is_task_mem_status_valid检查任务信息是否有效(进程是否已经结束)，
 *   助于判断无效则停止跟踪, 将任务信息移除链表
 *   此函数未再手动加锁， 只能用于临界区中检查时使用
 *   使用位置, 检查过程中 用于实时检查不合规的tms
 * Input Parameters:
 *  pid_t pid
 * Returned Value:
 *  return  0 ---> valid  else invalid
 ****************************************************************************/
static int clear_invalid_tms(void)
{
  irqstate_t flags;
  struct tcb_s *task = NULL;
  struct task_mem_stats *tms = NULL;
  struct task_mem_stats *temp = NULL;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry_safe(&task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    task = NULL;
    task = nxsched_get_tcb(tms->pid);
    if (!task)
    {
      DEBUG("该tms已经失效...正在删除\n");
      list_delete(&tms->node_task_mem);
      free(tms);
    }
    continue;
  }
  DEBUG("out \n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

/****************************************************************************
 * is_task_list_empty();
 * 要通过加锁的情况去判断是否链表为空， 如果为链表为空则返回1， 否则返回0
 ****************************************************************************/
static int is_task_list_empty(void)
{
  irqstate_t flags;
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in..\n");
  if (list_is_empty(&task_mem_status_list))
  {
    DEBUG("list is empty, nonthing to check\n");
    spin_unlock_irqrestore(&tms_list_lock, flags);
    return -1;
  }
  DEBUG("out..\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  return 0;
}

static void check_memory_leak(void)
{
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  if (is_task_list_empty())
  {
    DEBUG("Cant check memory leak since the list is null\n");
    return;
  }

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    int ucv;
    tms->count += 1;
    /** 此次检测运算权值 */
    ucv = cal_unfreed_count(tms);
    if (-1 != ucv)
    {
      INFO("ucv:%d", ucv);
    }
  }
  DEBUG("out \n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

/** 这里对应想办法通过文件管理系统来输出所有进程的信息, 暂时不用管 */
static void print_task_mem_stats(void)
{
  int i;
  char buffer[25] = {0};
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  if (is_task_list_empty())
  {
    DEBUG("nothing to print since the list is null\n");
    return;
  }

  flags = spin_lock_irqsave(&tms_list_lock);
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO, "\n================leak_info===============\n");
    syslog(LOG_INFO, "进程号:%u\n", tms->pid);
    syslog(LOG_INFO, "检测次数:%u\n", tms->count);
    syslog(LOG_INFO, "总分配次数:%u\n", tms->total_allocs);
    syslog(LOG_INFO, "未释放内存数:%u\n", tms->active_allocs);
    syslog(LOG_INFO, "分配总大小:%d\n", tms->total_size);
    syslog(LOG_INFO, "当前活跃内存量:%d\n", tms->active_size);
    syslog(LOG_INFO, "应用:%s\n", tms->appname);

    i = timestamp_to_utc_str(tms->timestamp, buffer, sizeof(buffer));
    if (!i)
    {
      syslog(LOG_INFO, "首次分配时间:%s\n", buffer);
    }
    syslog(LOG_INFO, "=======================================\n\n");
  }
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

static uint64_t temp;

static void leak_detection_worker(FAR void *arg)
{
  bool isEmpty;
  isEmpty = is_task_list_empty();

  /** 如果为空的话就不执行检查操作 */
  if (!isEmpty)
  {
    clear_invalid_tms();
    check_memory_leak();
    print_task_mem_stats();
  }
  work_queue(LPWORK,
             &g_leak_detection_work,
             leak_detection_worker,
             NULL,
             MSEC2TICK(5000));
  syslog(LOG_INFO, "%s例行遍历...%s\n", COLOR_TABLE[COLOR_MAGENTA], COLOR_TABLE[COLOR_RESET]);
}

/** 初始化该结构 */
void init_leak_detection(void)
{
  syslog(LOG_INFO, "%sinit_leak_detection...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(NULL);
}

/****************************************************************************
 *  add_metadata_to_task_mem_stats
 *  #define list_for_every_entry(list, entry, type, member)
 *  添加元数据信息
 *      1. 若对应进程的内存状态已经跟踪 则在原来的基础上修改
 *      2. 若未跟踪则通过create_task_mem_stat动态创建，修改信息
 ****************************************************************************/

static struct task_mem_stats *create_task_mem_stats(void)
{
  size_t len;
  struct task_mem_stats *p = NULL;

  len = sizeof(struct task_mem_stats);
  p = (struct task_mem_stats *)malloc(len);
  if (!p)
  {
    return NULL;
  }
  memset(p, 0, len);
  return p;
}
/****************************************************************************
 * Name: add_metadata_to_task_mem_stats
 *
 * Description:
 *    update task_mem_statas with metadata from memchecker
 * Input Parameters:
 *   metadata - struct memchecker_metadata *metadata
 *
 * Returned Value:
 *  return  0,  indicates  succeeding to updatate task_mem_statas or failing to update
 ****************************************************************************/
int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  pid = metadata->pid;
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == pid)
    {
      /** 内容暂时不全 */
      tms->total_allocs++;
      tms->active_allocs++;
      tms->total_size += metadata->size;
      tms->active_size += metadata->size;
      spin_unlock_irqrestore(&tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);

  /**  对应首次创建task_mem_stats 初始化 */
  tms = create_task_mem_stats();
  if (!tms)
  {
    DEBUG("tms:NULL\n");
    return -1;
  }
  tms->count = 0;
  tms->pid = pid;
  tms->total_allocs = 1;
  tms->active_allocs = 1;
  tms->total_size = metadata->size;
  tms->active_size += metadata->size;
  /** 直接记录创建时时间戳 */
  tms->timestamp = clock_systime_ticks();
  memcpy(tms->appname, metadata->file, 32);
  /** 初始化并插入链表中 */
  list_initialize(&tms->node_task_mem);
  flags = spin_lock_irqsave(&tms_list_lock);
  list_add_tail(&task_mem_status_list, &tms->node_task_mem);
  spin_unlock_irqrestore(&tms_list_lock, flags);
  return 0;
}
/****************************************************************************
 * Name: update_task_mem_stats_when_free
 *
 * Description:
 *    update task_mem_statas when free
 * Input Parameters:
 *   metadata - struct memchecker_metadata *metadata
 * free--(pid)---> update(size, active_alloc)
 *  既然是update 正在释放的时候被调用这个时候 显然就不可能出现
 *  不存在活跃任务的情况...
 * Returned Value:
 *  return  0,  indicates  succeeding to updatate task_mem_statas or failing to update
 ****************************************************************************/
int update_task_mem_stats_when_free(struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  pid = metadata->pid;
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == pid)
    {
      /** 内容暂时不全 */
      tms->active_allocs--;
      tms->total_size += metadata->size;
      tms->active_size -= metadata->size;
      spin_unlock_irqrestore(&tms_list_lock, flags);
      DEBUG("释放时tms信息修改成功!!!\n");
      DEBUG("out...\n");
      return 0;
    }
  }
  /** 如果存在某些不明原因没有找到pid 走此条路径 释放锁 */
  DEBUG("该任务已经无效, 低概率事件\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  return -1;
}