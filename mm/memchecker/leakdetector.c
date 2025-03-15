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
 * 用于调试输出
 ****************************************************************************/
#define LOG_FMT "%-16s: %-30u\n"
#define LOG_STR_FMT "%-16s: %-30s\n"
#define LOG_SIZE_FMT "%-16s: %-30lu\n"
#define SEPARATOR "========================================"
#define HIGH_FRE (5000)
#define LOW_FRE (20000)

/****************************************************************************
 *  此处取消钩子函数, 为避免嵌套造成问题，此文件中的内存申请操作不计入统计
 ****************************************************************************/
#undef malloc
#undef free

/****************************************************************************
 *  g_leak_detection_work 泄漏检查工作任务
 *  task_mem_status_list  检查链表
 ****************************************************************************/
/** 高频工作队列 */
atomic_t high_fre_workqueue = 0;

/** 低频工作队列 */
atomic_t low_fre_workqueue = 0;

extern struct memchecker_metadata metadata_list[];

static struct work_s hf_g_leak_detection_work;

static struct work_s lf_g_leak_detection_work;

/** 扫描频率较高 */
// struct list_node task_mem_status_list;

/**  扫描频繁较低  low_frequency*/
// struct list_node lowf_task_mem_status_list;
// spinlock_t tms_list_lock;

static struct task_stats_list_lock hf;

static struct task_stats_list_lock lf;

static int move_between_list(struct task_mem_stats *tms, struct task_stats_list_lock *cur_tsll);
int get_task_list_lock_hf(struct task_stats_list_lock **p)
{
  *p = &hf;
  syslog(LOG_INFO, "hf_list:%p\n", *p);
  return 0;
}

int get_task_list_lock_lf(struct task_stats_list_lock **p)
{
  *p = &lf;
  syslog(LOG_INFO, "lf_list:%p", *p);
  return 0;
}
/****************************************************************************
 * Name:  clear_invalid_tms
 *  Description:
 *  clear_invalid_tms通过tsll中链表遍历得到任务已经不运行的进程
 *  并且会清理对应的tms, 下次再进行检测的时候不会扫描到...
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

  /** isEmpty后面的内容 其实不是很必要*/
  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  isEmpty = list_is_empty(&tsll->task_mem_status_list);
  if (isEmpty)
  {
    INFO("此时链表为空, 无可更新的tms信息...\n");
    return;
  }
  DEBUG("in...\n");
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    task = nxsched_get_tcb(tms->pid);
    /** task == NULL  对应进程任务不存在 */
    if (!task)
    {
      DEBUG("tms对应进程已经不存在...正在删除\n");
      list_delete(&tms->node_task_mem);
      free(tms);
      continue;
    }
  }
  DEBUG("out \n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
}

/****************************************************************************
 * Name: is_task_list_empty
 *  Description:
 *  is_task_list_empty 针对的是无锁情况下的检测链表状态
 *  函数内存在申请锁释放锁的操作, 注意不要再加锁环境下再次调用此函数
 *  会产生死锁
 * Input Parameters:
 *  struct task_stats_list_lock *tsll
 * Returned Value:
 *       return -1
 ****************************************************************************/
static int is_task_list_empty(struct task_stats_list_lock *tsll)
{
  irqstate_t flags;

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in..\n");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
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
  uint64_t timestamp;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll can't be NULL, stop checking memory leak\n %s", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    syslog(LOG_INFO, "%sCant check memory leak since the list is null%s \n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
    DEBUG("out...\n");
    /** 由于链表为空, 故终止该链表的工作队列 */
    atomic_set_release(&tsll->workqueue_status, 0);
    syslog(LOG_INFO, "%s already changed the existing status %s \n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 计算此次检查时间戳 */
  timestamp = clock_systime_ticks();
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    /** 更新检查次数 */
    tms->count += 1;
    int ucv, wv_pos;
    int sum_weighted_value = 0;
    int i;

    wv_pos = tms->count - 1;
    ucv = cal_W1(tms);
    if (-1 == ucv)
    {
      INFO("权值计算,存在问题\n");
    }
    tms->weighted_value[wv_pos % 3] = ucv;
    /** 把后续的再完成一下 */
    /** 超越3次次数限制 */
    if (tms->count >= 3)
    {
      /** 只有高频链表才进行这样的判断
       *  高频与低频的差别在于权值高低 对应的移位顺序操作不同
       */
      if (tsll == &hf)
      {
        for (i = 0; i < 3; i++)
        {
          sum_weighted_value += tms->weighted_value[i];
        }
        if (sum_weighted_value < 60)
        {
          i = move_between_list(tms, tsll);
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
}

/** 这里对应想办法通过文件管理系统来输出所有进程的信息, 暂时不用管 */
static void print_task_mem_stats(struct task_stats_list_lock *tsll)
{
  int i;
  char buffer[25] = {0};
  struct task_mem_stats *tms = NULL;
  irqstate_t flags;

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    INFO("Cant print_task_list since the list is null\n");
    spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
    DEBUG("out...\n");
    return;
  }

  INFO("当前操作链表: %s\n", tsll == &hf ? "高频" : "低频");
  list_for_every_entry(&tsll->task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    syslog(LOG_INFO, "\n================leak_info===============\n");
    syslog(LOG_INFO, "进程号:%u\n", tms->pid);
    syslog(LOG_INFO, "检测次数:%u\n", tms->count);
    syslog(LOG_INFO, "总分配次数:%u\n", tms->total_allocs);
    syslog(LOG_INFO, "未释放内存数:%u\n", tms->active_allocs);
    syslog(LOG_INFO, "分配总大小:%d\n", tms->total_size);
    syslog(LOG_INFO, "当前活跃内存量:%d\n", tms->active_size);
    syslog(LOG_INFO, "应用:%s\n", tms->appname);
    /** 后续可能在这里添加时间戳 */
    syslog(LOG_INFO, "======================================\n\n");
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
}

static void leak_detection_worker(void *tsll_arg)
{
  /** 统计leak_detection_worker调用次数 */
  bool isEmpty;
  uint16_t timeout;
  irqstate_t flags;
  static int high_work_queue = 0;
  static int low_work_queue = 0;
  struct work_s *g_leak_detection_work;
  struct task_stats_list_lock *tsll = NULL;

  if (!tsll_arg)
  {
    syslog(LOG_WARNING, "%stsll传参为空,为避免故障,工作队列强行终止...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  tsll = (struct task_stats_list_lock *)tsll_arg;

  if (atomic_read_acquire(&tsll->workqueue_status) != 1)
  {
    if (tsll == &hf)
    {
      syslog(LOG_INFO, "%s高频链表为空, 暂停扫描...%s", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
      return;
    }
    else
    {
      syslog(LOG_INFO, "%s低频链表为空, 暂停扫描...%s", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
      return;
    }
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  isEmpty = list_is_empty(&tsll->task_mem_status_list);
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);

  if (!isEmpty)
  {
    /** 清除已经过期的任务 */
    clear_invalid_tms(tsll);
    /** 根据元数据信息检测内存泄漏 */
    check_memory_leak(tsll);
    /** 打印相关数据 */
    // print_task_mem_stats(tsll);
  }
  else
  {
    if (tsll == &hf)
    {
      syslog(LOG_INFO, "当前高频扫描链表为空, 停止扫描\n");
      atomic_set_release(&hf.workqueue_status, 0);
      return;
    }
    else
    {
      syslog(LOG_INFO, "当前低频扫描链表为空, 停止扫描\n");
      atomic_set_release(&lf.workqueue_status, 0);
      return;
    }
  }
  /** 对应判断工作队列所对应的路径 */
  if (tsll == &lf)
  {
    timeout = LOW_FRE;
    g_leak_detection_work = &lf_g_leak_detection_work;
    syslog(LOG_INFO, "低频链表扫描次数:%d...\n", ++high_work_queue);
  }
  else if (tsll == &hf)
  {
    timeout = HIGH_FRE;
    g_leak_detection_work = &hf_g_leak_detection_work;
    syslog(LOG_INFO, "高频链表扫描次数:%d...\n", ++low_work_queue);
  }

  work_queue(LPWORK,
             g_leak_detection_work,
             leak_detection_worker,
             (void *)tsll,
             MSEC2TICK(timeout));
}

void init_high_fre_leak_detection(void)
{
  /** 将状态初始化为1  0代表未启动 1代表已经启动 */
  atomic_set_release(&hf.workqueue_status, 1);
  syslog(LOG_INFO, "%s高频内存泄漏检测队列正在初始化...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&hf);
}

void init_low_fre_leak_detection(void)
{
  /** 设置读取状态 */
  atomic_set_release(&lf.workqueue_status, 1);
  syslog(LOG_INFO, "%s低频内存泄漏检测队列正在初始化...%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&lf);
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
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == pid)
    {
      /** 内容暂时不全 */
      tms->total_allocs++;
      tms->active_allocs++;
      tms->total_size += metadata->size;
      tms->active_size += metadata->size;
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  /**  对应首次创建task_mem_stats 初始化 */
  tms = create_task_mem_stats();
  if (!tms)
  {
    DEBUG("tms:NULL\n");
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
  memcpy(tms->appname, metadata->file, 32);
  /** 初始化并插入链表中 */
  list_initialize(&tms->node_task_mem);
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);
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
      INFO("释放时tms信息修改成功!!!\n");
      DEBUG("out...\n");
      return 0;
    }
  }
  /** 如果存在某些不明原因没有找到pid 走此条路径 释放锁 */
  WARN("free更新出错, 低概率事件, 应当注意\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return -1;
}

/** 供memchecker模块进行判断 函数中自带加锁 故禁止在锁环境中使用 */
int test_pid_in_tsll(pid_t pid)
{
  irqstate_t flags;
  struct task_mem_stats *tms = NULL;

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
    /** tms 存在于高频队列中 */
    if (tms->pid == pid)
    {
      spin_unlock_irqrestore(&lf.tms_list_lock, flags);
      return 1;
    }
  }
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  return -1;
}

/** 函数本身并未加锁 */
static int test_task_in_hf(pid_t pid)
{
}

/** 正处于哪个tsll当中  一共就只存在两个tsll  free 就全部消除*/
/** 就只有两种可能 去*/
static int move_between_list(struct task_mem_stats *tms, struct task_stats_list_lock *cur_tsll)
{
  irqstate_t flags;

  if (cur_tsll == &hf)
  {
    INFO("当前处于高频率工作队列,信息从高频移动到低频\n");
    list_delete(&tms->node_task_mem);
    flags = spin_lock_irqsave(&lf.tms_list_lock);
    list_add_tail(&lf.task_mem_status_list, &tms->node_task_mem);
    spin_unlock_irqrestore(&lf.tms_list_lock, flags);
    return 0;
  }
  else if (cur_tsll == &lf)
  {
    INFO("当前处于低频率工作队列,信息从低频移动到高频\n");
    list_delete(&tms->node_task_mem);
    flags = spin_lock_irqsave(&hf.tms_list_lock);
    list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
    spin_unlock_irqrestore(&hf.tms_list_lock, flags);
    return 1;
  }
  syslog(LOG_INFO, "%s传入的tsll为空, 存在巨大的安全隐患, 务必及时修复!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  return -1;
}
