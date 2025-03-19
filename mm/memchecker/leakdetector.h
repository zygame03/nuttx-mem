#ifndef __INCLUDE_NUTTX_MM_LEAKDETECTOR_H
#define __INCLUDE_NUTTX_MM_LEAKDETECTOR_H

#include <stdlib.h>
#include <stdio.h>
#include <nuttx/wqueue.h>
#include <nuttx/list.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

/****************************************************************************
 *  struct task_stats_list_lock
 *    该结构体封装了三个成员:
 *    @task_mem_status_list  对应task_mem_stats链表,
 *                           通过遍历链表中成员的信息检查异常
 *    @tms_list_lock         访问链表时所申请的锁, 保持数据一致性
 *    @workqueue_status      对应该工作队列的状态 (0--->未启用) (1--->启用)
 ****************************************************************************/
struct task_stats_list_lock
{
  struct list_node task_mem_status_list;
  spinlock_t tms_list_lock;
  atomic_t workequeue_status;
};

/****************************************************************************
 *  struct task_mem_stats
 *    该结构体通过malloc和free钩子函数对基本的内存分配信息进行整理得到的
 *    以进程为基准的内存申请泄漏的信息
 *    内存泄漏的具体行为通过所捕获的内存信息进行判断
 ****************************************************************************/
struct task_mem_stats
{
  uint16_t score; /**  进程实时得分分值,通过权值计算得到  ---限制其大小 */

  uint16_t weighted_value[3]; /** 记录最近三次的权值 最终取平均值 */

  uint32_t count; /**  对应进程检测次数 */

  char appname[32]; /** 对应运行的程序名 */

  uint64_t init_timestamp; /** 对应结构体初始化时间戳  */

  uint64_t check_timestamp; /** 每次检测会更改的时间戳   */

  pid_t pid; /** 任务 ID  */

  uint32_t total_allocs; /**该进程的总分配次数*/

  uint32_t active_allocs; /** 活跃的分配数量  */

  uint32_t total_size; /**  总共分配的大小   */

  uint32_t active_size; /** 仍然活跃的大小  */

  struct list_node node_task_mem; /*进程内存对象元数据链表 */
};

/****************************************************************************
 *  struct mm_standard_value
 *    此结构体内用于设置一些评判的内存泄漏的标准值, 标准值可以人为去设定
 ****************************************************************************/
struct mm_standard_value
{
  volatile uint32_t active_memory_alert_value;   /** 活跃内存警戒值 */
  volatile uint32_t memory_residency_alert_time; /** 内存驻留时间 */
};

/****************************************************************************
 * Public Function Definitions
 ****************************************************************************/
void init_leak_detection(void);

int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata);

int update_task_mem_stats_when_free(struct task_stats_list_lock *ttls, struct memchecker_metadata *metadata);

void get_task_list_lock_hf(struct task_stats_list_lock **p);

void get_task_list_lock_lf(struct task_stats_list_lock **p);

int test_pid_in_tsll(pid_t pid);

void init_high_fre_leak_detection(void);

void init_low_fre_leak_detection(void);
#endif
