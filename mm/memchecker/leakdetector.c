/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <sched.h>
#include <nuttx/mm/memchecker.h>
#include <nuttx/mm/leakdetector.h>
#include <syslog.h>
#include <nuttx/mutex.h>

/****************************************************************************
 * 变量定义
 ****************************************************************************/
struct list_node node_task_mem;

static struct work_s g_leak_detection_work;

/**
 * 可能用自旋锁 不用这个锁
 */
static mutex_t g_metadata_mutex;

/**
 *  元数据链表
 */
extern struct list_node metadata_list;

/**
 *  基于进程的内存信息管理链表
 */
struct list_node task_mem_status_list;

void leak_detection_worker(FAR void *arg)
{
  /* 1. 执行实际的内存检测逻辑（此处省略） */
  /* 先假设申请的内存大于10B 就触发报警*/
  struct memchecker_metadata *metadata;

  printf("================leak_warning===============\n");
  list_for_every_entry(&metadata_list, metadata,
                       struct memchecker_metadata, node)
  {
    if (metadata->size == 200)
    {
      printf(" --- 200 ---\n");
      printf("metadata->addr:%p\n", metadata->addr);
    }
    else if (metadata->size == 45)
    {
      printf(" --- 45 ---\n");
      printf("metadata->addr:%p\n", metadata->addr);
    }
  }

  /* 2. 重新提交工作，实现周期性触发 */
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
  syslog(LOG_INFO, "Leak检测已启动\n");
}