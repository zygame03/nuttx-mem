#ifndef __INCLUDE_NUTTX_MM_LEAKDETECTOR_H
#define __INCLUDE_NUTTX_MM_LEAKDETECTOR_H

#include <stdlib.h>
#include <stdio.h>
#include <nuttx/wqueue.h>
#include <nuttx/list.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

/****************************************************************************
 * ALLOWED_GAP_INIT 初始化时允许比较高的申请释放差
 * ALLOWED_GAP_RUNNING 运行时只允许3词释放差
 * 示例公式：风险值 = 未释放次数差 × 权重1 + 未释放总大小 × 权重2
 * 这个方面可能判断比较严谨  通过节拍数
 * 至少5次检查以后再去存储相关信息
 ****************************************************************************/
#define ALLOWED_GAP_INIT (5)
#define ALLOWED_GAP_RUNNING (3)
#define MAX_SIZE (100)

/****************************************************************************
 *  process
 *  对应判断检测出问题的 --->
 ****************************************************************************/
struct task_stats_list_lock
{
  struct list_node task_mem_status_list;
  spinlock_t tms_list_lock;
};

struct task_mem_stats
{
  uint8_t score; // 进程实时得分分值,通过权值计算得到  ---限制其大小

  /** 记录最近三次的权值 最终取平均值 */
  uint16_t weighted_value[3]; // 权值总和--- 可能通过此次得分与上次的差异来加强检测

  uint32_t count; // 对应进程检测次数

  char appname[32]; // 程序名

  uint64_t init_timestamp; // 每次检测时间

  uint64_t check_timestamp; // 每次检测时间

  pid_t pid; // 任务 ID

  uint32_t total_allocs; // 总分配次数

  uint32_t active_allocs; // 未释放的次数

  uint32_t total_size; // 总分配大小

  uint32_t active_size; // 活跃的大小

  struct list_node node_task_mem; // 进程内存对象元数据链表
};

/****************************************************************************
 * work_queue leak detecting
 ****************************************************************************/
void init_leak_detection(void);

int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata);

int update_task_mem_stats_when_free(struct task_stats_list_lock *ttls, struct memchecker_metadata *metadata);

int get_task_list_lock_hf(struct task_stats_list_lock **p);

int get_task_list_lock_lf(struct task_stats_list_lock **p);

int test_pid_in_tsll(pid_t pid);
#endif
