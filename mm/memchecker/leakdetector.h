#ifndef __INCLUDE_NUTTX_MM_LEAKDETECTOR_H
#define __INCLUDE_NUTTX_MM_LEAKDETECTOR_H

#include <stdlib.h>
#include <stdio.h>
#include <nuttx/wqueue.h>
#include <nuttx/list.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include "calcm.h"

#define HISTORY_SIZE (30)
#define OP_WINDOW_SIZE (30)
#define CHECKING_TIMES (30)

#define PROCESS_NUM (20)

/****************************************************************************
 *  struct task_stats_list_lock
 *    该结构体封装了三个成员:
 *    @task_mem_status_list  对应task_mem_stats链表
 *    @tms_list_lock         访问链表时所申请的锁, 保持数据一致性
 *    @workqueue_status      对应该工作队列的状态 (0--->未启用) (1--->启用)
 ****************************************************************************/
struct task_stats_list_lock
{
  struct list_node task_mem_status_list;
  spinlock_t tms_list_lock;
  int pid_arr[PROCESS_NUM]; /** 存储进程号 */
  int num;                  /** 对应工作队列所跟踪的进程数 */
  atomic_t workequeue_status;
};

typedef enum
{
  ALLOC_LOG,
  FREE_LOG,
  NONE_LOG
} op_type_t;

// 针对于 每一节点下的搜索
typedef struct
{
  size_t buffer[HISTORY_SIZE]; // 最近20次的内存大小
  uint8_t front;               // 当前写入位置
  uint8_t count;               // 当前有效数据量
  uint8_t head;                // 当前有效数据量
  /** 记住这里要进行初始化*/
} HistoryBytesQueue;

typedef struct
{
  op_type_t buffer[OP_WINDOW_SIZE]; // 最近30次的内存大小
  uint8_t front;                    // 当前写入位置
  uint8_t count;                    // 当前有效数据量
  uint8_t head;                     // 当前有效数据量
} OpQueue;

/****************************************************************************
 *  struct task_mem_stats
 *    该结构体通过malloc和free钩子函数对基本的内存分配信息进行整理得到的
 *    以进程为基准的内存申请泄漏的信息
 *    内存泄漏的具体行为通过所捕获的内存信息进行判断
 ****************************************************************************/
struct task_mem_stats
{
  struct list_node node_task_mem; /*进程内存对象元数据链表 */

  pid_t pid; /** 任务ID  */

  float score; /**  进程实时得分分值,通过权值计算得到  ---限制其大小 */

  /** 每一次check的时候去存储 */
  HistoryBytesQueue history_bytes; /** 最近20次字节变化记录  */

  OpQueue opq; /** 最近100次字节变化记录 */

  size_t total_mm_size;

  uint16_t total_alloc_count;

  uint32_t count; /**  对应进程检测次数 */

  uint8_t warning_count; /**  对应进程检测次数 */

  char appname[32]; /** 对应运行的程序名 */

  uint64_t init_timestamp; /** 对应结构体初始化时间戳  */

  /** 如果要用的话这里需要初始化 */
  spinlock_t tms_lock; /** 把tms导出来以后, 加锁判断 */

  uint64_t last_timestamp; /** 每次检测会更改的时间戳   */
};

struct rt_mem_info
{
  size_t total_active_mm_size; /** 活跃内存使用的总大小 */

  size_t max_mm_size; /** 最大内存块大小 */

  int unfreed_count; /** y未释放申请的次数 */

  clock_t max_mm_time; /** 内存块中存活的最长时间 */
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

// 权重因子结构体
typedef struct
{
  /** 未释放率引起的基本权重 */
  float w_leak_rate;
  /** 给出一个0.2的trend 基本值 */
  float w_trend;
  /**分配聚集度权重*/
  float w_clustering;
  /**内存年龄权重 */
  float w_age;

} weight_factors_t;

// 添加数据 这里其实可以直接优化 把它放在结构体的最先
void hb_queue_push(HistoryBytesQueue *q, size_t data);

// 获取第n秒前的数据（n=0表示最新） 历史字节数大小 查看增速
size_t hb_queue_get(const HistoryBytesQueue *q, uint8_t n);

// 添加新数据 获取历史 ___push metadata
void op_queue_push(OpQueue *q, op_type_t data);

// 获取历史操作记录
op_type_t op_queue_get(OpQueue *q, uint8_t n);

/****************************************************************************
 * Public Function Definitions
 ****************************************************************************/
void init_leak_detection(void);

int add_metadata_to_task_mem_stats(struct memchecker_metadata *metadata);

int update_task_mem_stats_when_free(struct memchecker_metadata *metadata);

void get_task_list_lock_hf(struct task_stats_list_lock **p);

void get_task_list_lock_lf(struct task_stats_list_lock **p);

int test_pid_in_tsll(pid_t pid);

void init_high_fre_leak_detection(void);

void init_low_fre_leak_detection(void);

int get_task_mm_info(struct task_mem_stats *tms, struct rt_mem_info *rt_info);

int get_info_by_pid(pid_t pid, struct rt_mem_info *rt_info);

void print_task_info_by_id(pid_t pid);

#endif
