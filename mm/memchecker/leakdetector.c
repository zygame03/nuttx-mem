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
#include <nuttx/clock.h>
#include <nuttx/lib/math.h>

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
 *  check_memory_leak();
 *  通过权值算法加权判断,权重如下
 *  存活时间权重（W1）
 *  未释放块的平均存活时间：avg_age = Σ(current_time - alloc_ts)/unreleased_count
 *  权重公式：W1 = log2(avg_age / 基准时间) （基准时间建议取10秒）
 *
 *  内存失衡权重（W2）
 *  申请/释放比例：alloc_free_ratio = (total_alloc - total_free) / (total_alloc + 1)
 *  权重公式：W2 = 1 / (1 + exp(-5*(alloc_free_ratio-0.3))) （Sigmoid强化突变）
 *
 *  内存增长斜率（W3）
 *  单位时间内存增长：mem_growth = (current_mem - prev_mem) / 检测间隔
 *  权重公式：W3 = tanh(mem_growth / 内存警戒线)
 *
 *  调用栈聚集度（W4）
 *  统计相同调用栈模式的未释放块占比
 *  权重公式：W4 = 最大重复调用栈比例 * 2
 *
 *  leak_score = 0.4*W1 + 0.3*W2 + 0.2*W3 + 0.1*W4
 ****************************************************************************/
static void test_float()
{
  float a1 = 3.1415;
  float a2 = 3.5656;
  float a3;
  float a4;
  uint64_t t1 = clock_systime_ticks();
  for (int i = 0; i < 50; i++)
  {
    a3 = a2 * a1;
    a4 = a2 / a1;
  }
  uint64_t t2 = clock_systime_ticks();
  uint64_t delta_ticks = t2 - t1;
  DEBUG("time: %lu\n", delta_ticks);
}

static void test_simu()
{
  // float a1 = 3.1415;
  uint16_t a1 = (31415 << 15) / 10000;
  uint16_t a2 = (35656 << 15) / 10000;
  uint16_t a3;
  uint16_t a4;
  uint64_t t1 = clock_systime_ticks();
  for (int i = 0; i < 50; i++)
  {
    a3 = a2 * a1;
    a4 = a2 / a1;
  }
  uint64_t t2 = clock_systime_ticks();
  uint64_t delta_ticks = t2 - t1;
  DEBUG("time: %lu\n", delta_ticks);
}

static uint64_t get_average_active_age(pid_t pid)
{
  return 50;
}

static void check_memory_leak()
{
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;
  double w1, w2, w3, w4;
  double avg_age, alloc_free_ratio, mem_growth; // 暂时还没有加上调用栈相关的

  // uint64_s
  // uint64_t
  test_float();
  test_simu();
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in ---check_memory_leak()...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    // 次数需要自增
    tms->count += 1;
    avg_age = get_average_active_age(tms->pid);
    // 此处基准时间设置为10s
    // w1 = log2ceil((avg_age / 10));
  }
  DEBUG("out ---check_memory_leak()...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

/** 这里对应想办法通过文件管理系统来输出所有进程的信息, 暂时不用管 */
static void print_task_mem_stats(void)
{
  int i;
  char buffer[32] = {0};
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("进入临界区...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO,
           "\n" SEPARATOR "\n"
           "  Leak Detection Report       \n" SEPARATOR "\n" LOG_FMT // 进程号
               LOG_FMT                                               // 检测次数
                   LOG_FMT                                           // 总分配次数
                       LOG_FMT                                       // 未释放内存数
                           LOG_SIZE_FMT                              // 分配总大小
           LOG_STR_FMT                                               // 应用名称
           "%s\n"                                                    // 时间戳（带格式判断）
           SEPARATOR "\n",
           "Process ID", tms->pid,
           "Check Count", tms->count,
           "Total Allocs", tms->total_allocs,
           "Active Allocs", tms->active_allocs,
           "Total Size", tms->total_size,
           "Application", tms->appname,
           (i = format_timestamp(tms->timestamp, buffer, 1)) ? "" : "First Alloc Time:  ", buffer);
  }
  DEBUG("退出临界区...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
}

// 可能需要选择使用多个结构体
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
    print_task_mem_stats();
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
  test_simu();
  test_float();
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 非首次创建 */
    if (tms->pid == pid)
    {
      tms->total_allocs++;
      tms->active_allocs++;
      tms->total_size += metadata->size;
      spin_unlock_irqrestore(&tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tms_list_lock, flags);
  // 对应首次创建task_mem_stats
  tms = create_task_mem_stats();
  if (!tms)
  {
    return -1;
  }
  memcpy(tms->appname, metadata->file, strlen(metadata->file));
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