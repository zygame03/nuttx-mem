/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <nuttx/nuttx/spinlock.h>
#include <syslog.h>
#include <nuttx/mutex.h>
#include <nuttx/clock.h>
#include <nuttx/lib/math.h>
#include "leakdetector.h"
#include "utils.h"
#include "mmdebug.h"

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

struct list_node task_mem_status_list;

static spinlock_t g_list_lock = SPIN_INITIALIZER;

/****************************************************************************
 *  check_memory_leak();
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
  char buffer[25] = {0};
  struct task_mem_stats *tms = NULL;

  // syslog(LOG_INFO, "%s此处未释放内存超出正常标准!%s", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO, "\n================leak_info===============\n");
    syslog(LOG_INFO, "进程号:%u\n", tms->pid);
    syslog(LOG_INFO, "检测次数:%u\n", tms->count);
    syslog(LOG_INFO, "总分配次数:%u\n", tms->total_allocs);
    syslog(LOG_INFO, "未释放内存数:%u\n", tms->active_allocs);
    syslog(LOG_INFO, "分配总大小:%lu\n", tms->total_size);
    // int timestamp_to_utc_str(uint64_t timestamp, char *buffer, size_t buf_size);

    i = timestamp_to_utc_str(tms->timestamp, buffer, sizeof(buffer));
    ////DEBUG("buffer:%s\n", buffer);
    if (!i)
    {
      syslog(LOG_INFO, "首次分配时间:%s\n", buffer);
    }
    syslog(LOG_INFO, "=======================================\n\n");
  }
}

static void leak_detection_worker(FAR void *arg)
{
  /* 1. 执行实际的内存检测逻辑（此处省略） */
  /* 先假设申请的内存大于10B 就触发报警*/
  print_task_mem_stats();
  /* 2. 重新提交工作，实现周期性触发 */
  check_memory_leak();
  work_queue(HPWORK,                 // 使用高优先级队列
             &g_leak_detection_work, // 工作结构体
             leak_detection_worker,  // 工作函数
             NULL,                   // 参数（可传递自定义数据）
             MSEC2TICK(5000));       // 5秒后再次执行
}
void init_leak_detection(void)
{
  /* 首次提交工作，启动周期性检测 */
  work_queue(HPWORK,
             &g_leak_detection_work,
             leak_detection_worker,
             NULL,
             MSEC2TICK(0)); // 立即执行（无延迟）
  syslog(LOG_INFO, "%s==============================================================\n",
         COLOR_TABLE[COLOR_GREEN]);
  syslog(LOG_INFO, "================Leak detection inited successfully=============\n");
  syslog(LOG_INFO, "==============================================================%s\n", COLOR_TABLE[COLOR_RESET]);
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
  list_initialize(&p->node_task_mem);
  list_add_tail(&task_mem_status_list, &p->node_task_mem);
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

  pid = metadata->pid;
  test_simu();
  test_float();
  flags = spin_lock_irqsave(&tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    if (tms->pid == pid)
    {
      // 不需要重新赋值pid
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
  tms->count = 0;
  tms->pid = pid;
  tms->total_allocs = 1;
  tms->active_allocs = 1;
  tms->total_size = metadata->size;
  tms->timestamp = metadata->ts;
  //////DEBUG("metadata->ts:%lu\n", metadata->ts);
  return 0;
}
