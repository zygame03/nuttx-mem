/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include "calcm.h"
#include "mmdebug.h"
#include <syslog.h>
#include "utils.h"

/** 活跃内存的基准大小   64 */
#define MEMORY_BLOCK_BASE MM_MEMCHECKER_DATA_SIZE
#define MEMORY_TIME_BASE 500

enum WEIGHT
{
  WEIGHT_UNFREED_COUNT,
  WEIGHT_UNFREED_SIZE_RATE,
  WEIGHT_AGE,
  WEIGHT_NUM
};

/**  指定权值 */
static int WEIGHT_TABLE[WEIGHT_NUM] = {
    [WEIGHT_UNFREED_COUNT] = 10,
    [WEIGHT_UNFREED_SIZE_RATE] = 5,
    [WEIGHT_AGE] = 10};

enum LEAK_ERR
{
  UNFREEED_NUM,
  UNFREEED_CHUNK,
  HIGH_GROWTH_RATE,
  LEAK,
};

void print_leak_err_info(enum LEAK_ERR err)
{
  switch (err)
  {
  case UNFREEED_NUM:
    syslog(LOG_INFO, "%s未释放的内存数量超出安全值!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case UNFREEED_CHUNK:
    syslog(LOG_INFO, "%s存在大量内存未释放!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case HIGH_GROWTH_RATE:
    syslog(LOG_INFO, "%s内存申请增速过快!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case LEAK:
    syslog(LOG_INFO, "%s内存存在泄露!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  }
}

void basic_test()
{
}

/****************************************************************************
 * Name: cal_unfreed_count
 *  计算方式: 未释放次数 * (10  +  (内存块大小 / 基准块大小))
 *  暂时定位64字节
 *  计算方式2: 未释放次数 * (10  + 2^ (活跃内存 / 活跃的基准警戒大小) )
 * Description:
 *      calculate weighed value with unfreed_count in task
 * Input Parameters:
 *   metadata - struct memchecker_metadata *metadata
 *
 * Returned Value:
 *  return  an integer,  indicates  succeeding to updatate task_mem_statas or failing to update
 *  return -1 , ---> fail
 ****************************************************************************/
int cal_unfreed_count(struct task_mem_stats *tms)
{
  /** 分别对应未释放次数， 未释放内存块大小，以及最终的权值 */
  int unfreed_count, unfreed_size, ufc_val;

  if (!tms)
  {
    WARN("传入的tms为空!!!\n");
    return -1;
  }
  /** 未释放次数 * (10  +  (内存块大小 / 基准块大小)) */
  unfreed_count = tms->active_allocs;
  unfreed_size = tms->active_size;

  ufc_val = (int)(unfreed_count * (WEIGHT_TABLE[WEIGHT_UNFREED_COUNT] + (float)unfreed_size / MEMORY_BLOCK_BASE));
  INFO("---权值计算过程输出---\n");
  INFO("unfreed_count: %d\n", unfreed_count);
  INFO("unfreed_size: %d\n", unfreed_size);
  INFO("计算公式: %d * (%d + (%d / %d)) =%d", unfreed_count, WEIGHT_TABLE[WEIGHT_UNFREED_COUNT], unfreed_size, MEMORY_BLOCK_BASE, ufc_val);

  return ufc_val;
}

/****************************************************************************
 * Name:cal_unfreed_chunck
 *  计算方式: 未释放次数 * (10  +  (内存块大小 / 基准块大小))
 *  暂时定位64字节
 *  计算方式2: 未释放次数 * (10  + 2^ (活跃内存 / 活跃的基准警戒大小) )
 * Description:
 *      calculate weighed value with unfreed_count in task
 * Input Parameters:
 *   metadata - struct memchecker_metadata *metadata
 *
 * Returned Value:
 *  return  an integer,  indicates  succeeding to updatate task_mem_statas or failing to update
 *  return -1 , ---> fail
 ****************************************************************************/
int cal_unfreed_chunck(struct task_mem_stats *tms)
{
  /** 获取内存的存活时间 * 内存的大小量 /  */
  /** 存活时间 * 大小 */
}
