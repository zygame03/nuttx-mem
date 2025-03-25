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
 *  高频检测时间 HIGH_FRE
 *  高频检测时间 LOW_FRE
 ****************************************************************************/
#define HIGH_FRE (100)
#define LOW_FRE (20000)

/****************************************************************************
 *  对应循环窗口中的插入操作
 *  op_queue_push  对应记录最近某次的OP_WINDOW_SIZE次
 *                  free 或 malloc行为,以此分析内存安全性
 *  hb_queue_push  记录历史多少次的活跃字节数
 *                 以此中记录的数据分析内存增长行为
 ****************************************************************************/
void op_queue_push(OpQueue *q, op_type_t data)
{
  if (!q)
  {
    syslog(LOG_WARNING, "%s opqueue is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  if (data != FREE_LOG && data != ALLOC_LOG)
  {
    syslog(LOG_WARNING, "%s wrong data type is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 存储数据 */
  q->buffer[q->front] = data;
  /** 计算下一次插入或覆盖位置 */
  q->front = (q->front + 1) % OP_WINDOW_SIZE;

  if (q->count < OP_WINDOW_SIZE)
  {
    q->count++;
  }
  // 统一计算head，无论队列是否满
  q->head = (q->front - q->count + OP_WINDOW_SIZE) % OP_WINDOW_SIZE;
}

void hb_queue_push(HistoryBytesQueue *q, size_t data)
{
  if (!q)
  {
    syslog(LOG_WARNING, "%s hbqueue is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  if (0 > data)
  {
    syslog(LOG_WARNING, "%s data can't be negative ...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  /** 存储数据 */
  q->buffer[q->front] = data;
  /** 计算下一次插入或覆盖位置 */
  q->front = (q->front + 1) % HISTORY_SIZE;

  if (q->count < HISTORY_SIZE)
  {
    q->count++;
  }
  // 统一计算head，无论队列是否满
  q->head = (q->front - q->count + HISTORY_SIZE) % HISTORY_SIZE;
}

size_t hb_queue_get(const HistoryBytesQueue *q, uint8_t n)
{
  if (!q)
  {
    syslog(LOG_WARNING, "%s hbqueue is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /*** 获取从head开始的第几位数据 */
  return q->buffer[(q->head + n) % HISTORY_SIZE];
}
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

/****************************************************************************
 * struct task_stats_list_lock @arg;  对应与链表访问相关的访问的元素
 *        @hf 高频
 *        @lf 低频
 *      对应高频、低频主要在于扫描基于元数据架构的信息判断其内存行为安全性的频率
 *      显然高频链表更加消耗系统资源, 故若当判定任务内存风险分较低会将其从高频链表
 *      移动至低频链表
 ****************************************************************************/
static struct task_stats_list_lock hf;

static struct task_stats_list_lock lf;

/***  导出链表(对应高频工作队列) */
void get_task_list_lock_hf(struct task_stats_list_lock **p)
{
  *p = &hf;
  syslog(LOG_INFO, "%s[%s()]:list(hf):%p ... %s\n",
         COLOR_TABLE[COLOR_BLUE], __func__, p, COLOR_TABLE[COLOR_RESET]);
  return;
}

/***  导出链表(对应低频工作队列) */
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
 *    如果进程已经执行完毕则不再继续进行跟踪
 *    此函数的作用是遍历链表清除已经无效的元数据节点
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
    syslog(LOG_WARNING, "%s @tsll can't be NULL ...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  /** 同时读取工作队列对应状态, 判断是否启用 , 若未启用此时链表必然为空*/
  if (!atomic_read_acquire(&tsll->workequeue_status))
  {
    syslog(LOG_WARNING, "%s workqueue isn't in use, which means the list is empty...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    task = nxsched_get_tcb(tms->pid);
    /** task == NULL  对应任务已经失效 , 此时将指针剔除链表, 并且free */
    if (!task)
    {
      syslog(LOG_INFO, "%sThe task_mem_stats(pid:%u) is no longer valid, deleting...%s\n",
             COLOR_TABLE[COLOR_BLUE], tms->pid, COLOR_TABLE[COLOR_RESET]);
      list_delete(&tms->node_task_mem);
      /** 数据清空, 并释放 */
      memset(tms, 0, sizeof(struct task_mem_stats));
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
 *                      不能在加锁环境中使用此函数
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

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in..\n");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    /** 对应检测下状态 ---额外检查修正, 确保功能正常 */
    if (atomic_read_acquire(&tsll->workequeue_status))
    {
      atomic_set_release(&tsll->workequeue_status, 0);
      syslog(LOG_WARNING, "%s Serious problems...\n %s",
             COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    }
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
  int count, i;
  struct rt_mem_info rt_info;
  struct task_mem_stats *tms = NULL;
  struct task_mem_stats *temp = NULL;
  struct task_stats_list_lock *dest_tsll = NULL;
  struct memchecker_metadata metadata_list[20] = {0};
  irqstate_t flags;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 进一步判断工作队列状态 */
  if (!atomic_read_acquire(&tsll->workequeue_status))
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
    /** 检测次数 时间戳 */
    tms->count++;
    tms->last_timestamp = clock_systime_ticks();
    memset(&rt_info, 0, sizeof rt_info);
    i = get_info_by_pid(tms->pid, &rt_info);
    if (i)
    {
      syslog(LOG_WARNING, "%sfailed to get info by pid...%s\n",
             COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
      continue;
    }
    /** --- 记录当下活字节 --- */
    hb_queue_push(&tms->history_bytes, rt_info.total_active_mm_size);
    i = basic_meomory_leak_check(&rt_info, tms);
    if (i)
    {
      // todo 这里初始化需要初始化为0
      tms->warning_count++;
      continue;
    }
    if (tms->count > CHECKING_TIMES)
    {
      float score = calculate_leak_score(rt_info, tms);
      if (score >= 1.f)
      {
        report_err(LEAK_DEFAULT_ERR, tms->pid);
        tms->warning_count++;
      }
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
}
/****************************************************************************
 * Name : print_task_mem_stats  print_todo
 *  Description:
 *          输出打印单独进程对应链表中存储的内存状态信息
 * Input Parameters:
 *          struct task_stats_list_lock *tsll
 * Returned Value:
 *       return 0 ===> 链表不为空
 *             -1 ===> 链表为空
 ****************************************************************************/
static void print_task_mem_stats(struct task_mem_stats *tms)
{
  struct rt_mem_info rt_info;
  int i;

  if (!tms)
  {
    syslog(LOG_WARNING, "%stms can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  i = get_info_from_metadata(tms, &rt_info);
  if (i)
  {
    syslog(LOG_INFO, "%s failed to get real-time info...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  syslog(LOG_INFO, "\n---------------\n");
  syslog(LOG_INFO, "task_id: %u \n", tms->pid);
  syslog(LOG_INFO, "app_name: %s \n", tms->appname);
  syslog(LOG_INFO, "monitoring_time: %u \n", (tms->last_timestamp - tms->init_timestamp));
  syslog(LOG_INFO, "active_size: %u \n", rt_info.total_active_mm_size);
  syslog(LOG_INFO, "unfreed_count: %u \n", rt_info.unfreed_count);
  syslog(LOG_INFO, "---------------\n");
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
  list_for_every_entry(&tsll->task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
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
  uint32_t timeout;
  irqstate_t flags;
  static int high_work_queue = 0;
  static int low_work_queue = 0;
  struct work_s *g_leak_detection_work;
  struct task_stats_list_lock *tsll = NULL;

  if (!tsll_arg)
  {
    syslog(LOG_WARNING, "%s@tsll is NULL can't be NULL in detecting leak...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  tsll = (struct task_stats_list_lock *)tsll_arg;
  /** 如果工作队列不在启动状态, 此时不允许启用  */
  if (!atomic_read_acquire(&tsll->workequeue_status))
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
  /** todo */
  check_memory_leak(tsll);
  if (tsll == &hf)
  {
    timeout = HIGH_FRE;
    g_leak_detection_work = &hf_g_leak_detection_work;
    syslog(LOG_INFO, "%sworkqueue running times(hf):%d...\n%s",
           COLOR_TABLE[COLOR_BLUE], ++high_work_queue, COLOR_TABLE[COLOR_RESET]);
  }
  else
  {
    timeout = LOW_FRE;
    g_leak_detection_work = &lf_g_leak_detection_work;
    syslog(LOG_INFO, "%sworkqueue running times(lf):%d...\n%s",
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
  if (atomic_read_acquire(&hf.workequeue_status))
  {
    syslog(LOG_INFO, "%s no need to restart workqueue(hf) ...%s\n",
           COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  atomic_set_release(&hf.workequeue_status, 1);
  syslog(LOG_INFO, "%shf workqueue is initializing...%s\n",
         COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&hf);
}

/**初始化低频工作队列 */
void init_low_fre_leak_detection(void)
{
  /** 保证不会多次启动 */
  if (atomic_read_acquire(&lf.workequeue_status))
  {
    syslog(LOG_INFO, "%s no need to restart workqueue(hf)...%s\n",
           COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 设置读取状态 */
  atomic_set_release(&lf.workequeue_status, 1);
  syslog(LOG_INFO, "%slf workqueue is initializing...%s\n",
         COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&lf);
}

static struct task_mem_stats *create_task_mem_stats(void)
{
  size_t len;
  struct task_mem_stats *p = NULL;

  len = sizeof(struct task_mem_stats);
  p = (struct task_mem_stats *)malloc(len);
  if (!p)
    return NULL;
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
 *  return  0  indicates  succeeding to updatate task_mem_statas
 *          or failing to update
 ****************************************************************************/
int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata)
{
  pid_t pid;
  struct task_mem_stats *tms = NULL;
  struct tcb_s *tcb = NULL;
  irqstate_t flags;

  if (!metadata)
  {
    syslog(LOG_WARNING, "%s metadata can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  pid = metadata->pid;
  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%sThe task(task_id:%u) doesn't exit now, serious problem...%s\n",
           COLOR_TABLE[COLOR_RED], tms->pid, COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 只有把结构体加进去了以后才能够 去看是否需要整体启动 */
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    if (tms->pid == pid)
    {
      /** 增添一些记录 */
      op_queue_push(&tms->opq, ALLOC_LOG);
      tms->total_mm_size += metadata->size;
      DEBUG("out...\n");
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      /** 正常情况下 此处的工作队列的状态是运行中...  */
      if (!atomic_read_acquire(&hf.workequeue_status))
      {
        syslog(LOG_INFO, "%s Something wrong, seirous problems...%s\n",
               COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
        return -1;
      }
      return 0;
    }
  }
  /**  对应首次创建task_mem_stats 初始化 */
  tms = create_task_mem_stats();
  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms:NULL\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  /**---  初始化历史记录窗口  ---*/
  memset(tms->history_bytes.buffer, 0, HISTORY_SIZE);
  tms->history_bytes.count = 0;
  tms->history_bytes.head = 0;
  tms->history_bytes.front = 0;

  /** 初始操作窗口记录 */
  memset(tms->opq.buffer, 0, OP_WINDOW_SIZE);
  tms->history_bytes.count = 0;
  tms->history_bytes.head = 0;
  tms->history_bytes.front = 0;

  tms->total_mm_size = 0;

  op_queue_push(&tms->opq, ALLOC_LOG);
  tms->total_mm_size += metadata->size;
  /** 初始检查 */
  tms->count = 0;
  tms->pid = pid;
  /** 直接记录创建时时间戳 */
  tms->init_timestamp = clock_systime_ticks();
  /**  初始化分值  */
  tms->score = 0;
  memcpy(tms->appname, tcb->name, strlen(tcb->name));
  /** 初始化节点 */
  list_initialize(&tms->node_task_mem);

  /** 插入 */
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);
  /** 添加完毕以后, 工作队列是否正在使用需要判断, 默认首先添加进入高频工作队列 */
  /** 若高频工作队列未被启动的话, 启动高频工作队列*/
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
 *  return  0,  表示成功更新
 *          1,  表示出现错误
 ****************************************************************************/
int update_task_mem_stats_when_free(struct memchecker_metadata *metadata)
{
  struct task_mem_stats *tms = NULL;
  struct tcb_s *tcb = NULL;
  irqstate_t flags;
  pid_t pid;

  if (!metadata)
  {
    syslog(LOG_WARNING, "%s @metadata is NULL, which is not allowed ... %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  /** 额外检查pid 是否存在 */
  pid = metadata->pid;
  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%sThe task(task_id:%u) doesn't exit, serious problem...%s\n",
           COLOR_TABLE[COLOR_RED], metadata->pid, COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == metadata->pid)
    {
      /** 历史字节数  在这里是不需要的*/
      // hb_queue_push(&tms->history_bytes, metadata->size);
      /** 操作数*/
      op_queue_push(&tms->opq, FREE_LOG);
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }

  flags = spin_lock_irqsave(&lf.tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == metadata->pid)
    {
      /** 对于释放后的重新添加 */
      // hb_queue_push(&tms->opq, FREE_LOG);
      op_queue_push(&tms->opq, FREE_LOG);
      spin_unlock_irqrestore(&lf.tms_list_lock, flags);
      DEBUG("out...\n");
      return 0;
    }
  }
  syslog(LOG_WARNING, "%sSomehow, couldn't task(pid: ) has been deleted before...%s",
         COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  DEBUG("out...\n");
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  return -1;
}

/****************************************************************************
 * Name: test_pid_in_tsll
 *
 * Description:
 *      通过进程号 查找工作队列具体位于哪个工作队列
 *      注意 请勿在链表加锁情况下使用此函数, 会引发死锁
 * Input Parameters:
 *      pid_t pid
 * Returned Value:
 *  return  0,  表示成功更新
 *          1,  表示出现错误
 ****************************************************************************/
int test_pid_in_tsll(pid_t pid)
{
  irqstate_t flags;
  struct task_mem_stats *tms = NULL;

  /** 如果两个工作队列都没有启动 */
  if (!atomic_read_acquire(&hf.workequeue_status) && !atomic_read_acquire(&lf.workequeue_status))
  {
    syslog(LOG_WARNING, "%sNo workqueue is in use...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_RESET);
    return -1;
  }
  /** 如何高频工作队列处于启动状态 */
  if (atomic_read_acquire(&hf.workequeue_status))
  {
    flags = spin_lock_irqsave(&hf.tms_list_lock);
    DEBUG("in...\n");
    list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
    {
      /** tms 存在于高频队列中 */
      if (tms->pid == pid)
      {
        DEBUG("out...\n");
        spin_unlock_irqrestore(&hf.tms_list_lock, flags);
        return 0;
      }
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  if (atomic_read_acquire(&lf.workequeue_status))
  {
    flags = spin_lock_irqsave(&lf.tms_list_lock);
    DEBUG("in...\n");
    list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
    {
      /** tms 存在于低频队列中 */
      if (tms->pid == pid)
      {
        DEBUG("out...\n");
        spin_unlock_irqrestore(&lf.tms_list_lock, flags);
        return 1;
      }
    }
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  return -1;
}

/****************************************************************************
 * Name :move_tms_to_hf_list  move_tms_to_lf_list
 *
 * Description:
 *      将对应的监管信息从高频队列, 低频队列之间移动...
 *      注意在移动过后相应要判断减少了tms的链表是否为空 已判定是否需要终止工作队列 避免空转
 * Input Parameters:
 *      pid_t pid
 * Returned Value:
 *  return  0,  表示成功更新
 *          1,  表示出现错误
 ****************************************************************************/
static int move_tms_to_hf_list(struct task_mem_stats *tms)
{
  irqstate_t flags;

  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...\n");
  /** 确保该节点已经初始化过了 */
  list_initialize(&tms->node_task_mem);
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  DEBUG("out...\n");
  spin_unlock_irqrestore(&hf.task_mem_status_list, flags);
  return 0;
}

static int move_tms_to_lf_list(struct task_mem_stats *tms)
{
  irqstate_t flags;

  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  flags = spin_lock_irqsave(&lf.tms_list_lock);
  DEBUG("in...\n");
  /** 确保该节点已经初始化过了 */
  list_initialize(&tms->node_task_mem);
  list_add_tail(&lf.task_mem_status_list, &tms->node_task_mem);
  DEBUG("out...\n");
  spin_unlock_irqrestore(&lf.task_mem_status_list, flags);
  return 0;
}

/****************************************************************************
 * Name:get_info_by_pid
 *
 * Description:
 *    实时获得进程的相关内存信息
 *    通过调用get_info_from_metadata
 * Input Parameters:
 * Returned Value:
 *  return  0,  indicates  succeeding to updatate task_mem_statas or failing to update
 *  整体用链表锁 导出tms 以后访问内部数据需要用tms_lock
 *  pid or tms  把tms 导出来内部再加锁 加锁再导出
 *  通过tms去导出链表
 *  作为参数都不能为NULL
 ******************************************************************************/
int get_info_by_pid(pid_t pid, struct rt_mem_info *rt_info)
{
  struct tcb_s *tcb = NULL;
  struct memchecker_metadata *metadata = NULL;
  struct list_node *node = NULL;
  int count, i;
  size_t total_active_mm_size = 0,
         max_mm_size = 0, total_mm_size = 0;
  int total_alloc_count = 0, unfreed_count = 0;
  clock_t active_mm_total_time = 0, max_mm_time = 0;
  irqstate_t tms_flags;

  if (!rt_info)
  {
    syslog(LOG_WARNING, "%srt_info can't be NULL ...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%s task doesn't exist now, serious problem...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  // todo
  struct memchecker_metadata metadata_list[20] = {0};
  count = pid_to_metadata(pid, &metadata_list);
  if (0 > count)
  {
    syslog(LOG_INFO, "%sserious problems...count < 0...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  for (i = 0; i < count; i++)
  {
    if (metadata_list[i].state == MEMCHECKER_ALLOCATED)
    {
      clock_t time;
      unfreed_count += 1;
      total_active_mm_size += metadata_list[i].size;
      max_mm_size = (metadata_list[i].size > max_mm_size) ? metadata_list[i].size : max_mm_size;
      time = clock_systime_ticks() - metadata_list[i].alloc_track.ts;
      active_mm_total_time += time;
      max_mm_time = (time > max_mm_time) ? time : max_mm_time;
    }
  }

  rt_info->total_active_mm_size = total_active_mm_size;
  rt_info->max_mm_size = max_mm_size;
  rt_info->unfreed_count = unfreed_count;
  rt_info->active_mm_total_time = active_mm_total_time;
  rt_info->max_mm_time = max_mm_time;
  return 0;
}
