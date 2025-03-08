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
struct task_mem_stats
{
  uint32_t score; // 进程实时得分分值

  uint32_t weighted_value; // 权值

  uint32_t count; // 对应进程检测次数  y

  // enum tstate_e state; // 对应进程状态

  uint64_t timestamp; // 对应首次内存对象分配时间戳

  pid_t pid; // 任务 ID y

  uint32_t total_allocs; // 总分配次数 y

  uint32_t active_allocs; // 未释放的次数 y

  uint64_t total_size; // 总分配大小 y

  struct list_node node_task_mem; // 进程内存对象元数据链表
};

/****************************************************************************
 * work_queue leak detecting
 ****************************************************************************/

void init_leak_detection(void);

int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata);

#endif
