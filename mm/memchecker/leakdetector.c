/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <nuttx/spinlock.h>
#include <syslog.h>
#include <math.h>
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
 *  高频检测间隔时间 HIGH_FRE (500 ~ 1s)
 *  低频检测间隔时间 LOW_FRE
 ****************************************************************************/
#define HIGH_FRE (1000)
#define LOW_FRE (20000)

/****************************************************************************
 *   @ALARM_THRESHOLD  报警阈值
 *   @MAX_ALRAM_NUMBER 报警次数上限
 ****************************************************************************/
#define ALARM_THRESHOLD (0.7f)
#define MAX_ALRAM_NUMBER (5)

/****************************************************************************
 *  此处取消钩子函数,为避免循环嵌套问题
 *  此文件中的内存操作不计入管理
 ****************************************************************************/
#undef malloc
#undef free

/****************************************************************************
 *  work_queue 工作队列
 *        @hf_g_leak_detection_work 对应高频的工作队列
 *        @lf_g_leak_detection_work 对应低频的工作队列
 ****************************************************************************/
static struct work_s hf_g_leak_detection_work;

static struct work_s lf_g_leak_detection_work;

/****************************************************************************
 * struct task_stats_list_lock @arg;  对应与链表访问相关的访问的元素
 *        @hf 高频
 *        @lf 低频
 *      对应高频、低频主要在于扫描基于元数据相关的信息判断其内存行为安全性的频率
 *      显然高频链表更加消耗系统资源, 故若当判定任务较为安全时移动至低频工作链表
 ****************************************************************************/
static struct task_stats_list_lock hf;

static struct task_stats_list_lock lf;
/****************************************************************************
 *  op_queue_push  对应记录最近OP_WINDOW_SIZE次
 *                  free 或 malloc数据, 借此分析内存安全行为
 *  hb_queue_push  记录历史活跃字节数
 *                 以此中记录的数据分析内存增长行为
 *  op_queue_get   按照排位获取静态循环队列记录的malloc\free 操作值
 *
 *  hb_queue_get   按照排位获取历史字节数 方便后续计算活跃内存增速
 ****************************************************************************/
op_type_t op_queue_get(OpQueue *q, uint8_t n)
{
  if (!q)
  {
    syslog(LOG_WARNING, "%s opqueue is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  return q->buffer[(q->head + n) % OP_WINDOW_SIZE];
}

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
    syslog(LOG_WARNING, "%s wrong data type is not allowed, serious problem...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /* 插入数据 并且计算下一个插入位置*/
  q->buffer[q->front] = data;
  q->front = (q->front + 1) % OP_WINDOW_SIZE;

  if (q->count < OP_WINDOW_SIZE)
  {
    q->count++;
  }
  /* 统一计算head，无论队列是否满*/
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
  /** 插入数据 */
  q->buffer[q->front] = data;
  q->front = (q->front + 1) % HISTORY_SIZE;

  if (q->count < HISTORY_SIZE)
  {
    q->count++;
  }
  /* 统一计算head，无论队列是否满*/
  q->head = (q->front - q->count + HISTORY_SIZE) % HISTORY_SIZE;
}

size_t hb_queue_get(const HistoryBytesQueue *q, uint8_t n)
{
  if (!q)
  {
    syslog(LOG_WARNING, "%s hbqueue is NULL, which is not allowed...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /*** 获取从head开始的第几位数据 */
  return q->buffer[(q->head + n) % HISTORY_SIZE];
}

/****************************************************************************
 *  相关操作函数声明
 ****************************************************************************/
static int move_tms_to_hf_list(struct task_mem_stats *tms);

static int move_tms_to_lf_list(struct task_mem_stats *tms);

static int move_between_list(struct task_stats_list_lock *tsll, struct task_mem_stats *tms);

static void clear_invalid_tms(struct task_stats_list_lock *tsll);

static void check_memory_leak(struct task_stats_list_lock *tsll);

static int is_task_list_empty(struct task_stats_list_lock *tsll);

static void print_task_mem_stats(struct task_mem_stats *tms);

/***  导出链表地址(对应高频工作队列) */
void get_task_list_lock_hf(struct task_stats_list_lock **p)
{
  *p = &hf;
  syslog(LOG_INFO, "%s[%s()]:list(hf):%p ... %s\n",
         COLOR_TABLE[COLOR_BLUE], __func__, p, COLOR_TABLE[COLOR_RESET]);
  return;
}

/***  导出链表地址(对应低频工作队列) */
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
    syslog(LOG_WARNING, "%s @tsll can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  if (!atomic_read_acquire(&tsll->workequeue_status))
  {
    syslog(LOG_WARNING, "%s workqueue isn't in use,  the list is empty...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  DEBUG("in-%s...", tsll == &hf ? "hf" : "lf");
  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    task = nxsched_get_tcb(tms->pid);
    /** task == NULL  对应任务已经失效 , 此时将指针剔除链表, 并且free */
    if (!task)
    {
      syslog(LOG_INFO, "%sThe task_mem_stats(pid:%u) is no longer valid, deleting...%s\n",
             COLOR_TABLE[COLOR_BLUE], tms->pid, COLOR_TABLE[COLOR_RESET]);
      list_delete_init(&tms->node_task_mem);
      /** 数据清空, 并释放 */
      memset(tms, 0, sizeof(struct task_mem_stats));
      free(tms);
    }
  }
  /** 清理过后判断链表是否为空, 对应是否终止工作队列 */
  isEmpty = list_is_empty(&tsll->task_mem_status_list);
  if (isEmpty)
  {
    syslog(LOG_INFO, "%s The linkedlist(%s) is empty now, changing status... %s",
           COLOR_TABLE[COLOR_BLUE], &hf == tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    /**链表为空 则更新工作队列状态 */
    atomic_set_release(&tsll->workequeue_status, 0);
  }
  DEBUG("out-%s...", tsll == &hf ? "hf" : "lf");
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
  DEBUG("in-%s", tsll == &hf ? "hf" : "lf");
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    /** 对应检测下状态 ---额外检查修正, 确保功能正常 */
    if (atomic_read_acquire(&tsll->workequeue_status))
    {
      atomic_set_release(&tsll->workequeue_status, 0);
      syslog(LOG_WARNING, "%s The workqueue shouldn't be in use, Serious problems...\n %s",
             COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    }
    DEBUG("out-%s", tsll == &hf ? "hf" : "lf");
    spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
    return -1;
  }
  DEBUG("out..\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return 0;
}
/****************************************************************************
 * Name: move_between_list 是否需要进行判别
 *  Description:
 *       工作队列定期通过此函数遍历链表, 通过当前实时信息结合历史记录
 *       判断是否发生了内存泄漏...
 * Input Parameters:
 *  struct task_stats_list_lock *tsll
 * Returned Value: None
 ***************************************************************************/
static int move_between_list(struct task_stats_list_lock *tsll, struct task_mem_stats *tms)
{
  int ret;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%stsll can't be NULL... %s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (!tms)
  {
    syslog(LOG_WARNING, "%stms can't be NULL... %s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (&hf == tsll)
  {
    if (!tms->warning_count && tms->count > CHECKING_TIMES)
    {
      list_delete_init(&tms->node_task_mem);
      ret = move_tms_to_lf_list(tms);
      if (ret)
      {
        WARN("something wrong when tms was moved to list(lf)...");
        return -1;
      }
      WARN("move to list(lf) sucessfully...");
      return 0;
    }
  }
  else
  {
    if (tms->warning_count)
    {
      list_delete_init(&tms->node_task_mem);
      ret = move_tms_to_hf_list(tms);
      if (ret)
      {
        WARN("something wrong when tms was moved to list(hf)...");
        return -1;
      }
      WARN("move to list(hf) sucessfully...");
      return 0;
    }
  }
}

/****************************************************************************
 *  warning_handler 根据对应的进程处理对应 报错情况
 ****************************************************************************/
static void warning_handler(pid_t pid, int count)
{
  struct tcb_s *tcb = NULL;
  int isDelete;

  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%s[task:%u] doesn't exit, check the pid, serious problem...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
  }

  /** 报警的次数 count */
  if (count < MAX_ALRAM_NUMBER)
  {
    syslog(LOG_WARNING, "%s[task:%u] has triggered memory leak warning (number:%u)...%s\n",
           COLOR_TABLE[COLOR_RED], pid, count, COLOR_TABLE[COLOR_RESET]);
    syslog(LOG_WARNING, "%stask would be forced to stop when warning number is over %u...%s\n",
           COLOR_TABLE[COLOR_RED], MAX_ALRAM_NUMBER, COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 达到次数上限 此时执行delete操作 */
  syslog(LOG_WARNING, "%s[task:%u] has already triggered memory leak waring %u (times)...%s\n",
         COLOR_TABLE[COLOR_RED], pid, MAX_ALRAM_NUMBER, COLOR_TABLE[COLOR_RESET]);
  isDelete = task_delete(pid);
  WARN("deleting [task:%u]", pid);
  if (ERROR == isDelete)
  {
    syslog(LOG_WARNING, "%ssomething wrong when deleting the task:%u...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    return;
  }
  return;
}
/****************************************************************************
 * Name: check_memory_leak
 *  Description:
 *       工作队列定期通过此函数遍历链表, 通过当前实时信息结合历史记录
 *       判断是否发生了内存泄漏...
 * Input Parameters:
 *  struct task_stats_list_lock *tsll
 * Returned Value: None
 ****************************************************************************/
static void check_memory_leak(struct task_stats_list_lock *tsll)
{
  int count, i, isErr;
  struct rt_mem_info rt_info;
  struct task_mem_stats *tms = NULL;
  struct task_mem_stats *temp = NULL;
  struct task_stats_list_lock *dest_tsll = NULL;
  irqstate_t flags;

  if (!tsll)
  {
    syslog(LOG_WARNING, "%s@tsll is NULL, which is not allowed...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  if (!atomic_read_acquire(&tsll->workequeue_status))
  {
    syslog(LOG_WARNING, "%swokequeue(%s) isn't in use...\n %s",
           COLOR_TABLE[COLOR_RED], &hf == tsll ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return;
  }
  DEBUG("in...");
  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  list_for_every_entry_safe(&tsll->task_mem_status_list, tms, temp, struct task_mem_stats, node_task_mem)
  {
    // print_task_mem_stats(tms); 基本没有大问题
    /** 检测次数 时间戳  内存实时状态信息获取记录(存储在rt_info中)*/
    tms->count++;
    tms->last_timestamp = clock_systime_ticks();
    memset(&rt_info, 0, sizeof(struct rt_mem_info));
    i = get_info_by_pid(tms->pid, &rt_info);
    if (i)
    {
      syslog(LOG_WARNING, "%sfailed to get info by pid...%s\n",
             COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
      continue;
    }
    /** todo 记录历史活跃字节  这里需要限制活跃空闲块的数量  */
    hb_queue_push(&tms->history_bytes, rt_info.total_active_mm_size);

#ifndef DISABLE_INFO
    /** 历史活跃字节记录总量 */
    count = tms->history_bytes.count;
    INFO("--------history_bytes_head:%u--------", tms->history_bytes.head);
    INFO("-----------pid:%u app:%s----------", tms->pid, tms->appname);
    /** 调试输出  */
    for (i = 0; i < count; i++)
    {
      INFO("history[%u] = %d",
           i, hb_queue_get(&tms->history_bytes, i));
    }
    count = tms->opq.count;
    INFO("------------------------------------\n");

    INFO("--------history_opq_head:%d--------", tms->opq.head);
    for (i = 0; i < count; i++)
    {
      switch (op_queue_get(&tms->opq, i))
      {
      case 0:
        INFO("ALLOC");
        break;
      case 1:
        INFO("FREE");
        break;
      case -1:
        INFO("NONE");
        break;
      }
    }
    INFO("-----------------------------------\n");
#endif

    /** 在此处出了问题 以后需要直接去移动 */
    isErr = basic_meomory_leak_check(&rt_info, tms);
    if (isErr)
    {
      tms->warning_count++;
      WARN("[task:%u]Basic Err happend...", tms->pid);
      warning_handler(tms->pid, tms->warning_count);
      goto move;
    }
    else
      INFO("[task:%u]No Basic Err So Far...", tms->pid);
    if (tms->count > CHECKING_TIMES)
    {
      const float weighted_value = calculate_leak_score(&rt_info, tms);
      /** 根据报警 判断 */
      INFO("weighted_value:%.2f", weighted_value);
      if (isgreaterequal(weighted_value, ALARM_THRESHOLD))
      {
        tms->warning_count++;
        report_err(LEAK_DEFAULT_ERR, tms->pid);
        warning_handler(tms->pid, tms->warning_count);
      }
    }
  move:
    move_between_list(tsll, tms);
  }
  /** 移动过后首先要判断高频链表是否为空 */
  if (list_is_empty(&tsll->task_mem_status_list))
  {
    atomic_set_release(&tsll->workequeue_status, 0);
    syslog(LOG_INFO, "%sLinkedlist(%s) is empty now...%s\n",
           COLOR_TABLE[COLOR_RED],
           tsll == &hf ? "hf" : "lf",
           COLOR_TABLE[COLOR_RESET]);
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);

  /** 链表不空 也不处于运行状态，此时进行初始化  */
  if (!atomic_read_acquire(&lf.workequeue_status) && !is_task_list_empty(&lf))
  {
    init_low_fre_leak_detection();
  }
  if (!atomic_read_acquire(&hf.workequeue_status) && !is_task_list_empty(&hf))
    init_high_fre_leak_detection();
  return;
}
/****************************************************************************
 * Name : print_task_mem_stats
 *  Description:
 *        输出打印某个进程中对应的活跃内存状态信息
 *        注意此函数需要在加锁环境下进行
 * Input Parameters:
 *          struct task_mem_stats *tms
 * Returned Value:
 *      No return
 ****************************************************************************/
static void print_task_mem_stats(struct task_mem_stats *tms)
{
  struct rt_mem_info rt_info = {0};
  int i, j;

  if (!tms)
  {
    syslog(LOG_WARNING, "%stms can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  i = get_info_by_pid(tms->pid, &rt_info);
  if (i)
  {
    syslog(LOG_WARNING, "%s failed to get real-time info...\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  printf(
      "%s"
      "+---------------------+----------------+\n"
      "| BASIC MEMORY INFO   | Value          |\n"
      "+---------------------+----------------+\n"
      "| Task ID             | %-14u |\n"
      "| App Name            | %-14s |\n"
      "| Score               | %-12.2f |\n"
      "| Warning Count       | %-14d |\n"
      "| Monitoring Time     | %-7u(ticks) |\n"
      "| Active Size         | %-11u(B) |\n"
      "| Unfreed Count       | %-14u |\n"
      "+---------------------+----------------+"
      "%s",
      COLOR_TABLE[COLOR_GREEN],
      tms->pid,
      tms->appname,
      tms->score,
      tms->warning_count, /* 报警次数*/
      (tms->last_timestamp - tms->init_timestamp),
      rt_info.total_active_mm_size,
      rt_info.unfreed_count,
      COLOR_TABLE[COLOR_RESET]);

  /**  历史字节（单列全宽展示） */
  printf(
      "+-----------------------------------------+\n"
      "| HISTORY BYTES(B)                        |\n"
      "+-----------------------------------------+");
  for (i = tms->history_bytes.head; i < tms->history_bytes.count;)
  {
    char buffer[256] = {0};
    int offset = snprintf(buffer, sizeof(buffer), "| ");

    // 每行打印 5个数据（根据总宽度38字符计算）
    for (j = 0; j < 5 && i < tms->history_bytes.count; j++, i++)
    {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%-6u ",
                         hb_queue_get(&tms->history_bytes, i));
    }

    // 右侧填充空格对齐
    int used_width = offset - 2;        // 扣除首部的"| "
    int remain_space = 40 - used_width; // 总宽度40 - "| "和尾部" |" -> 38
    snprintf(buffer + offset, sizeof(buffer) - offset, "%*s|", remain_space, "");
    syslog(LOG_INFO, "%s", buffer);
  }

  // 操作记录（单列全宽展示）
  printf(
      "+-----------------------------------------+\n"
      "| OPERATION RECORD                        |\n"
      "+-----------------------------------------+");
  for (i = tms->opq.head; i < tms->opq.count;)
  {
    char buffer[256] = {0};
    int offset = snprintf(buffer, sizeof(buffer), "| ");

    // 每行打印8个操作（每个操作占5字符）
    for (j = 0; j < 5 && i < tms->opq.count; j++, i++)
    {
      offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%-5s",
                         op_queue_get(&tms->opq, i) == 0 ? "ALLOC " : (op_queue_get(&tms->opq, i) == 1 ? "FREE" : "NONE"));
    }

    // 右侧填充空格对齐
    int used_width = offset - 2;
    int remain_space = 38 - used_width;
    snprintf(buffer + offset, sizeof(buffer) - offset, "%*s  |", remain_space, "");
    syslog(LOG_INFO, "%s", buffer);
  }
  printf("+-----------------------------------------+\n");
}

/****************************************************************************
 * Name : print_task_info_by_id
 *  Description: 通过pid打印对应进程任务的相关内存信息情况
 * Input Parameters:
 *      pid_t pid
 * Returned Value:
 *      No return
 ****************************************************************************/
void print_task_info_by_id(pid_t pid)
{
  irqstate_t flags;
  struct task_mem_stats *tms = NULL;
  struct tcb_s *tcb = NULL;

  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%s[task:%u] doesn't exit %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }

  /**  对应高频链表中搜寻 */
  if (atomic_read_acquire(&hf.workequeue_status))
  {
    flags = spin_lock_irqsave(&hf.tms_list_lock);
    DEBUG("in---(hf)...\n");
    list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
    {
      if (pid == tms->pid)
      {
        print_task_mem_stats(tms);
        DEBUG("out---(hf)");
        spin_unlock_irqrestore(&hf.tms_list_lock, flags);
        return;
      }
    }
    DEBUG("out---(hf)");
    spin_unlock_irqrestore(&hf.tms_list_lock, flags);
  }

  /**  对应低频链表中搜寻 */
  if (atomic_read_acquire(&lf.workequeue_status))
  {
    flags = spin_lock_irqsave(&lf.tms_list_lock);
    DEBUG("in---(lf)...\n");
    list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
    {
      if (pid == tms->pid)
      {
        print_task_mem_stats(tms);
        DEBUG("out---(lf)");
        spin_unlock_irqrestore(&lf.tms_list_lock, flags);
        return;
      }
    }
    DEBUG("out---(lf)");
    spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  }
  syslog(LOG_WARNING, "%s[task:%u] doesn't exist, failed to print task with PID...%s\n",
         COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
  return;
}

/*** 打印整个链表的内容 */
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
  if (!atomic_read_acquire(&tsll->workequeue_status))
  {
    syslog(LOG_WARNING, "%swokequeue(%s) isn't in use...\n %s",
           COLOR_TABLE[COLOR_RED], tsll == &hf ? "hf" : "lf", COLOR_TABLE[COLOR_RESET]);
    return;
  }

  flags = spin_lock_irqsave(&tsll->tms_list_lock);
  DEBUG("in...\n");
  list_for_every_entry(&tsll->task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    print_task_mem_stats(tms);
  }
  DEBUG("out...\n");
  spin_unlock_irqrestore(&tsll->tms_list_lock, flags);
  return;
}

static void print_all_task_mem_stats(void)
{
  print_task_list_stats(&hf);
  print_task_list_stats(&lf);
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
  if (!atomic_read_acquire(&tsll->workequeue_status))
    return;

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
    syslog(LOG_INFO, "%s no need to restart workqueue(hf)...%s\n",
           COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 数据清0 */
  hf.num = 0;
  memset(hf.pid_arr, 0, sizeof(hf.pid_arr));
  atomic_set_release(&hf.workequeue_status, 1);
  syslog(LOG_INFO, "%shf workqueue is initializing...%s\n",
         COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
  leak_detection_worker(&hf);
}

/**初始化低频工作队列 */
void init_low_fre_leak_detection(void)
{
  /** 防止多次启动 */
  if (atomic_read_acquire(&lf.workequeue_status))
  {
    syslog(LOG_INFO, "%s no need to restart workqueue(hf)...%s\n",
           COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  /** 初始化信息状态 */
  lf.num = 0;
  memset(lf.pid_arr, 0, sizeof(lf.pid_arr));
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
 * add_metadata_to_task_mem_stats   这里还需要去判断添加的时候是在哪个链表里面
 *
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

  /**  整体判断进程是否存在 */
  pid = metadata->pid;
  tcb = nxsched_get_tcb(pid);
  if (!tcb)
  {
    syslog(LOG_WARNING, "%sThe task(task_id:%u) doesn't exit now, serious problem...%s\n",
           COLOR_TABLE[COLOR_RED], tms->pid, COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  /**1. 是否存在于高频检查 */
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...");

  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 此分值表示对应进程 内存状态已经被跟踪 */
    if (tms->pid == pid)
    {
      /** 操作记录*/
      op_queue_push(&tms->opq, ALLOC_LOG);
      INFO("add ALLOC_LOG");
      tms->total_mm_size += metadata->size;
      /** 确保工作队列正在运行当中...  */
      if (!atomic_read_acquire(&hf.workequeue_status))
      {
        syslog(LOG_WARNING, "%s The workqueue(hf) should be running right now, something wrong...%s\n",
               COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
      }
      /** 添加总分配数 */
      tms->total_alloc_count++;
      syslog(LOG_WARNING, "%s[task:%u] has already updated tms in hf...%s",
             COLOR_TABLE[COLOR_RED],
             tms->pid,
             COLOR_TABLE[COLOR_RESET]);
      DEBUG("out");
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      return 0;
    }
  }
  DEBUG("out");
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);

  /** 2. 是否存在于低频*/
  flags = spin_lock_irqsave(&lf.tms_list_lock);
  DEBUG("in-lf...");
  list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 此分值表示对应进程 内存状态已经被跟踪 */
    if (tms->pid == pid)
    {
      /** 操作记录*/
      op_queue_push(&tms->opq, ALLOC_LOG);
      INFO("add ALLOC_LOG");
      tms->total_mm_size += metadata->size;
      /** 确保工作队列正在运行当中...  */
      if (!atomic_read_acquire(&lf.workequeue_status))
      {
        syslog(LOG_WARNING, "%s The workqueue(lf) should be running right now, something wrong...%s\n",
               COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
      }
      /** 添加总分配数 */
      tms->total_alloc_count++;
      INFO("[task:%u] has already updated tms in lf...", tms->pid);
      DEBUG("out");
      spin_unlock_irqrestore(&lf.tms_list_lock, flags);
      return 0;
    }
  }

  DEBUG("out");
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);

  /** 链表并不存在首次 添加到高频链表中*/
  flags = spin_lock_irqsave(&hf.tms_list_lock);
  DEBUG("in...");
  /**  高低频链表都扫描一遍后 仍然没有 对应首次创建task_mem_stats 初始化 */
  tms = create_task_mem_stats();
  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms:NULL\n %s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  /**  初始化申请释放记录历史窗口  */
  memset(tms->history_bytes.buffer, -1, HISTORY_SIZE);
  tms->history_bytes.count = 0;
  tms->history_bytes.head = 0;
  tms->history_bytes.front = 0;

  /** 初始化活跃字节历史记录 */
  memset(tms->opq.buffer, NONE_LOG, OP_WINDOW_SIZE);
  tms->history_bytes.count = 0;
  tms->history_bytes.head = 0;
  tms->history_bytes.front = 0;

  /** 初始化进程申请的总大小 和 报警次数 */
  tms->total_mm_size = 0;
  tms->warning_count = 0;
  tms->total_alloc_count = 1;

  INFO("add ALLOC_LOG");
  op_queue_push(&tms->opq, ALLOC_LOG);
  tms->total_mm_size += metadata->size;
  /** 初始检查 */
  tms->count = 0;
  tms->pid = pid;
  /** 直接记录创建时时间戳 */
  tms->init_timestamp = clock_systime_ticks();
  /**  初始化分值  */
  tms->score = 0.0f;
  memcpy(tms->appname, tcb->name, strlen(tcb->name));
  /** 初始化节点 */
  list_initialize(&tms->node_task_mem);

  /** 插入 */
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  syslog(LOG_INFO, "%s[task:%u] has been created and added to linkedlist(hf)... %s",
         COLOR_TABLE[COLOR_GREEN], tms->pid, COLOR_TABLE[COLOR_RESET]);
  DEBUG("out...");
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
  DEBUG("in(hf)...\n");
  list_for_every_entry(&hf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == metadata->pid)
    {
      /** 历史字节数  在这里是不需要的*/
      // hb_queue_push(&tms->history_bytes, metadata->size);
      /** 操作数*/
      op_queue_push(&tms->opq, FREE_LOG);
      INFO("add FREE_LOG");
      spin_unlock_irqrestore(&hf.tms_list_lock, flags);
      DEBUG("out(hf)...\n");
      return 0;
    }
  }
  /*** 如果内部没有把锁释放在外部进行释放 */
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);
  DEBUG("out(hf)...\n");

  flags = spin_lock_irqsave(&lf.tms_list_lock);
  DEBUG("in(lf)...\n");
  list_for_every_entry(&lf.task_mem_status_list, tms, struct task_mem_stats, node_task_mem)
  {
    /** 已经存在该线程的初始信息状态 */
    if (tms->pid == metadata->pid)
    {
      /** 对于释放后的重新添加 */
      // hb_queue_push(&tms->opq, FREE_LOG);
      op_queue_push(&tms->opq, FREE_LOG);
      spin_unlock_irqrestore(&lf.tms_list_lock, flags);
      DEBUG("out(lf)...\n");
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
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
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
  DEBUG("in(hf)...\n");
  /** 初始化该节点 */
  list_initialize(&tms->node_task_mem);
  list_add_tail(&hf.task_mem_status_list, &tms->node_task_mem);
  DEBUG("out(hf)...\n");
  syslog(LOG_INFO, "%s [task:%u] has already been moved to linkedlist(hf)...%s",
         COLOR_TABLE[COLOR_RED],
         tms->pid,
         COLOR_TABLE[COLOR_RESET]);
  spin_unlock_irqrestore(&hf.tms_list_lock, flags);
  return 0;
}

/**  */
static int move_tms_to_lf_list(struct task_mem_stats *tms)
{
  irqstate_t flags;

  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  DEBUG("in(lf-lock)...\n");
  flags = spin_lock_irqsave(&lf.tms_list_lock);
  /** 确保该节点已经初始化过了 */
  // list_initialize(&tms->node_task_mem);
  list_add_tail(&lf.task_mem_status_list, &tms->node_task_mem);
  spin_unlock_irqrestore(&lf.tms_list_lock, flags);
  DEBUG("out(lf-lock)...\n");
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
  size_t total_active_mm_size = 0, max_mm_size = 0, total_mm_size = 0;
  int unfreed_count = 0;
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
  // todo  待限制
  struct memchecker_metadata *metadata_list[20] = {0};
  count = pid_to_metadata(pid, metadata_list);
  if (0 > count)
  {
    syslog(LOG_INFO, "%sserious problems...count < 0...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  for (i = 0; i < count; i++)
  {
    if (metadata_list[i]->state == MEMCHECKER_ALLOCATED)
    {
      clock_t time;
      unfreed_count += 1;
      total_active_mm_size += metadata_list[i]->size;
      INFO("size of each memory block:%u(B)\n", metadata_list[i]->size);
      max_mm_size = (metadata_list[i]->size > max_mm_size) ? metadata_list[i]->size : max_mm_size;
      time = clock_systime_ticks() - metadata_list[i]->alloc_track.ts;
      active_mm_total_time += time;
      max_mm_time = (time > max_mm_time) ? time : max_mm_time;
    }
  }

  rt_info->total_active_mm_size = total_active_mm_size;
  rt_info->max_mm_size = max_mm_size;
  rt_info->unfreed_count = unfreed_count;
  rt_info->max_mm_time = max_mm_time;
  return 0;
}
