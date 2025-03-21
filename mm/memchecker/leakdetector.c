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
#include <nuttx/atomic.h>
#include "leakdetector.h"
#include "utils.h"
#include "mmdebug.h"
#include "calcm.h"

/****************************************************************************
 *  格式化输出
 ****************************************************************************/
#define MM_INFO_SEPARATOR "|--------------------------|\n"
#define HF_TASK_MM_INFO "|-- Task Memory Info(hf) --|\n"
#define LF_TASK_MM_INFO "|-- Task Memory Info(lf) --|\n"

/****************************************************************************
 *  对应高频 低频工作队列的检测时间
 *  1000 ====> 1s
 ****************************************************************************/
#define HIGH_FRE (10000)
#define LOW_FRE (20000)

/****************************************************************************
 *  此处取消钩子函数, 为避免嵌套造成问题，此文件中的内存申请操作不计入统计
 ****************************************************************************/
#undef malloc
#undef free

/****************************************************************************
 *  work_queue 工作队列定义
 *        @hf_g_leak_detection_work 对应高频的工作队列
 *        @lf_g_leak_detection_work 对应低频的工作队列
 ****************************************************************************/
static struct work_s hf_g_leak_detection_work;

static struct work_s lf_g_leak_detection_work;

static struct work_s warn_g_leak_detection_work;

/****************************************************************************
 * struct task_stats_list_lock @arg;  对应与链表访问相关的访问的元素
 *        @hf 高频
 *        @lf 低频
 *      对应高频、低频主要在于扫描基于元数据架构的信息判断其内存行为安全性的频率
 *      显然高频链表更加消耗系统资源, 故若当判定任务内存安全以后会将其从高频链表
 *      移动至低频链表
 ****************************************************************************/
static struct task_stats_list_lock hf;

static struct task_stats_list_lock lf;

static int move_between_list(struct task_mem_stats *tms, struct task_stats_list_lock *, struct task_stats_list_lock *);

/*** 导出链表(对应高频工作队列) */
void get_task_list_lock_hf(struct task_stats_list_lock **p)
{
  *p = &hf;
  syslog(LOG_INFO, "%s[%s()]:list(hf):%p ... %s\n",
         COLOR_TABLE[COLOR_BLUE], __func__, p, COLOR_TABLE[COLOR_RESET]);
  return;
}

/*** 导出链表(对应低频工作队列) */
void get_task_list_lock_lf(struct task_stats_list_lock **p)
{
  *p = &lf;
  syslog(LOG_INFO, "%s[%s()]:list(lf):%p ...\n %s",
         COLOR_TABLE[COLOR_BLUE], __func__, p, COLOR_TABLE[COLOR_RESET]);
  return;
}

/****************************************************************************
 * Name:  clear_invalid_tms
 * Description:
 *    清理已经不必要的跟踪, 如进程已经不存在了
 * Input Parameters:
 *  struct task_stats_list_lock *tsll
 * Returned Value:
 *      No Return
 ****************************************************************************/
static void clear_invalid_tms(struct task_stats_list_lock *tsll)
{
  irqstate_t flags;
  bool isEmpty;
  struct tcb_s *task = NULL;
  struct task_mem_stats *tms = NULL;
  struct task_mem_stats *temp = NULL;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s @tsll is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 同时读取工作队列对应状态, 判断是否启用 */
  if ((!atomic_read_acquire(&tsll->workequeue_status)))
  {
    syslog(LOG_WARNING, "%s workqueue isn't in use...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    task = nxsched_get_tcb(tms->pid);
    /** task == NULL  对应任务已经失效 */
    if (!task)
    {
      syslog(LOG_INFO, "%s The task(pid: ) is no longer valid, deleting...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
      list_delete(&tms->node_task_mem);
      free(tms);
    }
  }
  /*** 清理过后判断链表是否为空, 对应是否终止工作队列 */
  isEmpty = list_is_empty(&tsll->task_mem_status_list);
  if (isEmpty)
  {
    syslog(LOG_INFO, "%s The linkedlist(%s) is empty now, changing status... %s",
           COLOR_TABLE[COLOR_BLUE], &hf == tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    /**链表为空 则更新工作队列状态 */
    atomic_set_release(&tsll->workequeue_status, 0);
  }
  DEBUG("out \n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return;
}

/****************************************************************************
 * Name: is_task_list_empty
 *  Description:
 *  is_task_list_empty  检查链表本身是否为空...
 *                      注意不要在加锁环境中使用此函数
 *                      并且只有在工作队列处于启用状态下才能使用 todo(这里是否要加状态判断)
 * Input Parameters:
 *  struct task_stats_list_lock *tsll
 * Returned Value:
 *       return 0 ===> 链表不为空
 *             -1 ===> 链表为空
 ****************************************************************************/
static int is_task_list_empty(struct task_stats_list_lock *tsll)
{
  irqstate_t flags;

  if ((!tsll))
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in..\n");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    /** 对应检测下状态 ---额外检查修正, 确保功能正常 */
    if (atomic_read_acquire(&tsll->workequeue_status))
      atomic_set_release(&tsll->workequeue_status, 0);
    DEBUG("out..\n");
    spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
    return -1;
  }
  DEBUG("out..\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return 0;
}

static void check_memory_leak(struct task_stats_list_lock *tsll)
{
  struct task_mem_stats *tms = NULL;
  struct task_mem_stats *temp = NULL;
  struct task_stats_list_lock *dest_tsll = NULL;
  irqstate_t flags;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll ie NULL, which is not allowed...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  if ((!atomic_read_acquire(&tsll->workequeue_status)))
  {
    syslog(LOG_WARNING, "%swokequeue(%s) isn't in use...\n %s",
           COLOR_TABLE[COLOR_RED], &hf == tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  /** 计算此次检查时间戳 */
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    int val1, pos;
    int sum_weighted_value = 0;
    int i;

    /** 此处对元数据做出一些更改 */
    tms->count += 1;
    tms->check_timestamp = clock_systime_ticks();
    /** 由于统计最近3次的 分数值此处的 [pos%3]对应每次检测的分数值 */
    pos = tms->count - 1;

    /**  权值计算逻辑 */
    val1 = cal_w1(tms);
    if (-1 == val1)
    {
      syslog(LOG_WARNING, "%s权值计算存在故障\n%s",
             COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    }
    tms->weighted_value[pos % 3] = val1;

    /** 检测次数 >= 3次   ===> 看得分区间段是否正常
     * 如果得分比较低的话 移入低频检测链表
     *  此处逻辑待完善
     */
    if (tms->count >= 3)
    {
      if (tsll == &hf)
      {
        for (i = 0; i < 3; i++)
        {
          sum_weighted_value += tms->weighted_value[i];
        }
        if (sum_weighted_value < 60)
        {
          i = move_between_list(tms, &lf, tsll);
          if (-1 == i)
          {
            syslog(LOG_INFO, "%s移动链表内容失败...请尽快排查错误%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
          }
          syslog(LOG_INFO, "%s内容移动到低频链表成功...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
          /***在这时 需要查看低频链表是否处于启动状态 */
          if (atomic_read_acquire(&lf.tms_list_lock))
          {
            syslog(LOG_INFO, "%s低频链表扫描尚未启动...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
            init_low_fre_leak_detection();
          }
        }
      }
    }
  }
  DEBUG("out \n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return;
}

/****************************************************************************
 * Name : print_task_mem_stats
 *  Description:
 *          输出打印单独进程对应链表中存储的内存状态信息
 * Input Parameters:
 *          struct task_stats_list_lock *tsll
 * Returned Value:
 *       return 0 ===> 链表不为空
 *             -1 ===> 链表为空
 ****************************************************************************/
/** 根据时间戳的影响进行排序 */
/** void qsort(void *base, size_t nmemb, size_t size,
                  int (*compar)(const void *, const void *));

} */

static void mm_metadata_sort(struct memchecker_metadata **buffer, int len)
{
  int i, j;

  if (!buffer || 0 > len)
  {
    syslog(LOG_WARNING, "%s @buffer is NULL or length is negative...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
}

static void print_basic_mm_metadata(struct memchecker_meatadata *mm_metadata)
{
  int i, j;
  if (!mm_metadata)
  {
    syslog(LOG_WARNING, "%s @mm_metadata can't bu NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
}

static void print_task_mem_stats(pid_t pid)
{
  /** 输入某个进程号, 单独去打印某个进程下的内存使用状态 */
  /** 获得pid 但是实际上还是要去寻找  */
  /** 最近三次的权值计算情况 */
  /** 如果报错的话把他提到报警链表中 */
  /** todo here */
  struct tcb_s *tcb = NULL;
  int i, len;
  struct memchecker_metadata *buffer[CONFIG_MM_MEMCHECKER_PAGE_NUMBER] = {0};

  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%stask:%u doesn't exist...\n%s",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    return;
  }
  len = pid_to_metadata(pid, &buffer);
  mm_metadata_sort(&buffer, len);
  for (i = 0; i < len; i++)
  {
    print_basic_mm_metadata(buffer[i]);
  }
  /*** 这里打印对应进程的内存分配信息 */
  return;
}

/*** 输出整个链表 */
static void print_task_list_stats(struct task_stats_list_lock *tsll)
{
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  if ((!atomic_read_acquire(&tsll->workequeue_status)))
  {
    syslog(LOG_WARNING, "%swokequeue(%s) isn't in use...\n %s",
           COLOR_TABLE[COLOR_RED], tsll == &hf ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  syslog(LOG_INFO, HF_TASK_MM_INFO);
  list_for_every_entry(&tsll->task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO,
           "%s\n"
           "  |- task_pid: %u\n         "
           "  |- total_allocs : %d\n    "
           "  |- active_allocs: %d\n    "
           "  |- total_size: %d(B)\n    "
           "  |- active_size: %d(B)\n   "
           "  |- security_score: %d\n   "
           "  |- running_time: %ld(s)\n " MM_INFO_SEPARATOR
           "%s",
           COLOR_TABLE[COLOR_BLUE],
           tms->pid,
           tms->total_allocs,
           tms->active_allocs,
           tms->total_size,
           tms->active_size,
           tms->score,
           (tms->check_timestamp - tms->init_timestamp) / 500,
           COLOR_TABLE[COLOR_RESET]);
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return;
}

static void print_all_task_mem_stats(void)
{
  print_task_mem_stats(&hf);
  print_task_mem_stats(&lf);
  return;
}

static void leak_detection_worker(void *tsll_arg)
{
  bool isEmpty;
  uint16_t timeout;
  irqstate_t flags;
  static int high_work_queue = 0;
  static int low_work_queue = 0;
  struct work_s *g_leak_detection_work;
  struct task_stats_list_lock *tsll = NULL;

  if ((!tsll_arg))
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  tsll = (struct task_stats_list_lock *)tsll_arg;
  if ((!atomic_read_acquire(&tsll->workequeue_status)))
  {
    syslog(LOG_WARNING, "%swokequeue(%s) isn't in use...\n %s",
           COLOR_TABLE[COLOR_RED], &hf == tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return;
  }

  /** 清除已经过期的任务 */
  clear_invalid_tms(tsll);
  /** 如果此时链表已经为空 则直接返回, 无需再次检查 */
  if (is_task_list_empty(tsll))
    return;
  check_memory_leak(tsll);
  /** 打印相关数据 */
  // print_task_mem_stats(tsll);
  /** 对应判断工作队列所对应的路径 */
  if (tsll == &hf)
  {
    timeout = HIGH_FRE;
    g_leak_detection_work = &hf_g_leak_detection_work;
    syslog(LOG_INFO, "%schecking times(hf):%d...\n%s",
           COLOR_TABLE[COLOR_BLUE], ++high_work_queue, COLOR_TABLE[COLOR_RESET]);
  }
  else
  {
    timeout = LOW_FRE;
    g_leak_detection_work = &lf_g_leak_detection_work;
    syslog(LOG_INFO, "%schecking times(lf):%d...\n%s",
           COLOR_TABLE[COLOR_BLUE], ++low_work_queue, COLOR_TABLE[COLOR_RESET]);
  }

  work_queue(LPWORK,
             g_leak_detection_work,
             leak_detection_worker,
             (void *)tsll,
             MSEC2TICK(timeout));
}

/**初始化高频工作队列 */
void init_high_fre_leak_detection(void)
{
  /** 将状态初始化为1  0代表未启动 1代表已经启动 */
  atomic_set_release(&hf.workequeue_status, 1);
  syslog(LOG_INFO, "%shf workqueue is initializing...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&hf);
}

/**初始化低频工作队列 */
void init_low_fre_leak_detection(void)
{
  /** 设置读取状态 */
  atomic_set_release(&lf.workequeue_status, 1);
  syslog(LOG_INFO, "%slf workqueue is initializing...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&lf);
}

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
 *      从memchecker中对应的申请逻辑中插入此函数
 *      用于统计进程的内存使用情况
 * Input Parameters:
 *   metadata - struct memchecker_metadata *metadata
 *
 * Returned Value:
 *  return  0  indicates  succeeding to updatate task_mem_statas or failing to update
 *
 ****************************************************************************/
int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  struct tcb_s *tcb;
  irqstate_t flags;

  if (!metadata)
  {
    syslog(LOG_WARNING, "%s metadata can't be NULL...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  pid = metadata->pid;
  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%sThe task(task_id:%u) doesn't exit...%s\n",
           COLOR_TABLE[COLOR_RED], tms->pid, COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 只有把结构体加进去了以后才能够 去看是否需要整体启动 */
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态,
     * 如果是这种情况的话，
     * 很明显就不用再去查看状态了, 很明显有内容了  */
    if (tms->pid == pid)
    {
      /** 内容暂时不全 */
      tms->total_allocs++;
      tms->active_allocs++;
      tms->total_size += metadata->size;
      tms->active_size += metadata->size;
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      DEBUG("out...\n");
      /** 正常情况下 此处的工作队列的状态是运行中...  */
      if ((!atomic_read_acquire(&hf.workequeue_status)))
      {
        syslog(LOG_INFO, "%s Something wrong...%s\n",
               COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
        return -1;
      }
      return 0;
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  /**  对应首次创建task_mem_stats 初始化 */
  tms = create_task_mem_stats();
  if (!tms)
  {
    syslog(LOG_INFO, "%s tms:NULL\n %s", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 初始检查 */
  tms->count = 0;
  tms->pid = pid;
  tms->total_allocs = 1;
  tms->active_allocs = 1;
  tms->total_size = metadata->size;
  tms->active_size += metadata->size;
  /** 直接记录创建时时间戳 */
  tms->init_timestamp = clock_systime_ticks();
  /**  初始化分值  */
  tms->score = 0;
  memcpy(tms->appname, tcb->name, 32);
  /** 初始化并插入链表中 */
  list_initialize(&tms->node_task_mem);
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  /** 添加完毕以后, 工作队列是否正在使用需要判断 */
  if (!atomic_read_acquire(&hf.workequeue_status))
  {
    init_high_fre_leak_detection();
  }
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
int update_task_mem_stats_when_free(struct task_stats_list_lock *tsll, struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  if ((!tsll))
  {
    syslog(LOG_WARNING, "%s @tsll is NULL, which is not allowed ... %s", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if ((!metadata))
  {
    syslog(LOG_WARNING, "%s @metadata is NULL, which is not allowed ... %s", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (!atomic_read_acquire(&tsll->workequeue_status))
  {
    syslog(LOG_WARNING, "%s workqueue(%s) is not in use... %s",
           COLOR_TABLE[COLOR_RED], tsll == &hf ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  pid = metadata->pid;
  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&tsll->task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == pid)
    {
      /** 内容暂时不全 */
      tms->active_allocs--;
      tms->active_size -= metadata->size;
      spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }
  /** 如果存在某些不明原因没有找到pid 走此条路径 释放锁 */
  syslog(LOG_WARNING, "%sSomehow, couldn't task(pid: ) has been deleted before...%s", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return -1;
}

/** 供memchecker模块进行判断 函数中自带加锁 故禁止在锁环境中使用 */
int test_pid_in_tsll(pid_t pid)
{
  irqstate_t flags;
  struct task_mem_stats *tms = NULL;

  if ((!atomic_read_acquire(&hf.workequeue_status) &&
       !atomic_read_acquire(&lf.workequeue_status)))
  {
    return -1;
  }

  flags = spin_lock_irqsave(&hf.tms_list_lock);
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** tms 存在于高频队列中 */
    if (tms->pid == pid)
    {
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      return 0;
    }
  }
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  flags = spin_lock_irqsave(&lf.tms_list_lock);
  list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** tms 存在于低频队列中 */
    if (tms->pid == pid)
    {
      spin_unlock_irqrestore(&lf.tms_list_lock, flags);
      return 1;
    }
  }
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  return -1;
}

/** 正处于哪个tsll当中  一共就只存在两个tsll  free 就全部消除*/
/** 就只有两种可能 去*/
static int move_between_list(struct task_mem_stats *tms, struct task_stats_list_lock *dest_tsll, struct task_stats_list_lock *cur_tsll)
{
  irqstate_t flags;

  if (!tms)
  {
    syslog(LOG_WARNING, "%s@tms is NULL, which is not allowed... \n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (!cur_tsll || !dest_tsll)
  {
    syslog(LOG_WARNING, "%s@cur_tsll or @dest_tsll is NULL, which is not allowed... \n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 当前的工作队列需要处于运行状态, 否则容易出现严重问题*/
  if ((!atomic_read_acquire(&cur_tsll->workequeue_status)))
  {
    syslog(LOG_WARNING, "%s workqueue(%s) is not in use ... \n%s",
           COLOR_TABLE[COLOR_RED], &hf == cur_tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  syslog(LOG_INFO, "%s Detection of the task(ID : %u) is moving from %s %s..\n",
         COLOR_TABLE[COLOR_BLUE],
         tms->pid,
         cur_tsll == &hf ? "hf to lf" : "lf to hf",
         COLOR_TABLE[COLOR_RESET]);

  flags = spin_lock_irqsave(&cur_tsll->tms_list_lock);
  DEBUG("in...\n");
  list_delete(&tms->node_task_mem);
  /** 删除之后可能造成链表的状态为空 此时需要更新工作队列的状态 */
  if (list_is_empty(&cur_tsll->task_mem_status_list))
  {
    atomic_set_release(&cur_tsll->workequeue_status, 0);
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&cur_tsll->tms_list_lock, flags);

  /** 移动进入目标链表 */
  flags = spin_lock_irqsave(&dest_tsll->tms_list_lock);
  DEBUG("in...\n");
  list_add_tail(&dest_tsll->task_mem_status_list, &tms->node_task_mem);
  DEBUG("out...\n");
  spin_unlock_irqrestore(&cur_tsll->tms_list_lock, flags);
  /** 对应工作队列如果未启动 init */
  if (!atomic_read_acquire(&dest_tsll->workequeue_status))
  {
    if (dest_tsll == &lf)
      init_low_fre_leak_detection();
    else
      init_high_fre_leak_detection();
  }

  if (!atomic_read_acquire(&cur_tsll->workequeue_status))
    atomic_set_release(&cur_tsll->workequeue_status, 0);
  return 0;
}
