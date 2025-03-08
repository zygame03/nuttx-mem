/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <nuttx/mm/leakdetector.h>
#include <nuttx/mm/utils.h>
#include <nuttx/spinlock.h>
#include <nuttx/mm/mmdebug.h>
#include <syslog.h>
#include <nuttx/mutex.h>

/****************************************************************************
 *  此处取消钩子函数, 为避免嵌套造成问题，此文件中的内存申请操作不计入统计
 ****************************************************************************/
#undef malloc
#undef free

/****************************************************************************
 *  extern struct memchecker_metadata metadata_list[]; 单个内存对象中的元数据列表
 *  static struct work_s g_leak_detection_work;  工作结构体
 *  struct list_node task_mem_status_list; 任务内存状态列表
 *  volatile spinlock_t tms_list_lock;  对应列表对应的锁
 ****************************************************************************/
extern struct memchecker_metadata metadata_list[];

static struct work_s g_leak_detection_work;

struct list_node task_mem_status_list;

volatile spinlock_t tms_list_lock;

/****************************************************************************
 *  check_memory_leak(); 暂时放一放  xxxxx
 ****************************************************************************/
static void check_memory_leak()
{
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    tms->count++;
    if (tms->active_allocs > 5)
    {
      WARN("进程 %lu存在过多未释放的内存,可能存在内存泄漏!\n");
    }

    if (tms->total_size > 40)
    {
      WARN("进程 %lu申请的内存过大,可能存在内存泄漏!\n");
    }
  }
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

/** 这里对应想办法通过文件管理系统来输出所有进程的信息, 暂时不用管 */
static void print_task_mem_stats(void)
{
  int i;
  char buffer[25] = {0};
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO, "\n================leak_info===============\n");
    syslog(LOG_INFO, "进程号:%u\n", tms->pid);
    syslog(LOG_INFO, "检测次数:%u\n", tms->count);
    syslog(LOG_INFO, "总分配次数:%u\n", tms->total_allocs);
    syslog(LOG_INFO, "未释放内存数:%u\n", tms->active_allocs);
    syslog(LOG_INFO, "分配总大小:%lu\n", tms->total_size);
    i = timestamp_to_utc_str(tms->timestamp, buffer, sizeof(buffer));
    if (!i)
    {
      syslog(LOG_INFO, "首次分配时间:%s\n", buffer);
    }
    syslog(LOG_INFO, "=================leak_info=============\n\n");
  }
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

static void leak_detection_worker(FAR void *arg)
{
  bool isEmpty;
  spinlock_t flags;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  isEmpty = list_is_empty(&task_mem_status_list);
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  DEBUG("Task_List:%s\n", isEmpty ? "empty" : "not empty");
  if (!isEmpty)
  {
    // print_task_mem_stats();
    /* 2. 重新提交工作，实现周期性触发 */
    check_memory_leak();
    DEBUG("leak_detection_worker is working right now...\n");
    work_queue(HPWORK,                 // 使用高优先级队列
               &g_leak_detection_work, // 工作结构体
               leak_detection_worker,  // 工作函数
               NULL,                   // 参数（可传递自定义数据）
               MSEC2TICK(5000));       // 5秒后再次执行
  }
  else
  {
    DEBUG("The Linkedlist is empty, leak detection ceased...\n");
  }
}

// 这函数感觉其实没有什么用
// void init_leak_detection(void)
// {
//   /** 3s后开始执行 挂起3s后开始执行 */
//   work_queue(HPWORK,
//              &g_leak_detection_work,
//              leak_detection_worker,
//              NULL,
//              MSEC2TICK(3000));
// }

/****************************************************************************
 *  add_metadata_to_task_mem_stats();
 *  添加元数据信息
 *      1. 若对应进程的内存状态已经跟踪 则在原来的基础上修改
 *      2. 若未跟踪则通过create_task_mem_stat动态创建，修改信息
 ****************************************************************************/

/** 创建任务内存信息结构体 */
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

int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  pid = metadata->pid;
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 非首次创建 */
    if (tms->pid == pid)
    {
      tms->total_allocs++;
      tms->active_allocs++;
      tms->total_size += metadata->size;
      spin_unlock_irqrestore(&tms_list_lock, flags);
      DEBUG("退出临界区...\n");
      return 0;
    }
  }
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  // 对应首次创建task_mem_stats
  tms = create_task_mem_stats();
  if (!tms)
  {
    return -1;
  }
  tms->count = 0;
  tms->pid = pid;
  tms->total_allocs = 1;
  tms->active_allocs = 1;
  tms->total_size = metadata->size;
  tms->timestamp = metadata->ts;
  list_initialize(&tms->node_task_mem);
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  list_add_tail(&task_mem_status_list, &tms->node_task_mem);
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  leak_detection_worker(NULL);
  return 0;
}

int update_task_mem_stats_by_free(struct memchecker_metadata *metadata)
{
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");

  /** 找到链表中的数据进行修改 */
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    if (tms->pid == metadata->pid)
    {
      tms->active_allocs--;
      tms->total_size -= metadata->size;
    }
  }
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}