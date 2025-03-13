/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include "calcm.h"
#include "mmdebug.h"
#include <syslog.h>
#include "utils.h"
#include <nuttx/lib/math.h>

#define MEMORY_ACTIVE_SIZE CONFIG_MM_MEMCHECKER_DATA_SIZE
#define MEMORY_TIME_BASE 500
/** 内存的生存周期时间 20s  */
#define ACTIVE_TIME_PERIOD 20

enum WEIGHT
{
  WEIGHT_UNFREED_COUNT,
  WEIGHT_CHUNCK_AND_SIZE,
  WEIGHT_AGE,
  WEIGHT_NUM
};

/**  指定权值 */
static int WEIGHT_TABLE[WEIGHT_NUM] = {
    [WEIGHT_UNFREED_COUNT] = 8,
    [WEIGHT_CHUNCK_AND_SIZE] = 30,
    [WEIGHT_AGE] = 10};

enum LEAK_ERR
{
  UNFREEED_NUM,
  UNFREEED_CHUNK,
  HIGH_GROWTH_RATE,
  LEAK,
  LEAK_ERR_NUM
};

void print_leak_err_info(enum LEAK_ERR err)
{
  switch (err)
  {
  case UNFREEED_NUM:
    syslog(LOG_INFO, "%s未释放的内存数量超出安全值!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case UNFREEED_CHUNK:
    syslog(LOG_INFO, "%s存在大量未释放的内存!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case HIGH_GROWTH_RATE:
    syslog(LOG_INFO, "%s内存申请增速过快!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case LEAK:
    syslog(LOG_INFO, "%s内存存在泄露的情况!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  }
}

int is_basic_err(struct task_mem_stats *tms)
{
  /** 此时表示一定存在有未释放的内存 */
  if (!tms->active_allocs && tms->active_size)
  {
    print_leak_err_info(LEAK);
    WARN("此时存在严重的安全问题\n");
    return -1;
  }
  /** 返回0 表示不存在基础性的问题 */
  return 0;
}

/****************************************************************************
 * Name: cal_unfreed_count
 *
 *  Description:
 *  计算方式:
 *    额外权值:活跃内存 / 活跃的基准警戒大小
 *    未释放次数 * (10  + 2^ (额外权值) )
 *
 * Input Parameters:
 *   tms -  struct task_mem_stats *tms
 *
 * Returned Value:
 *   成功, 返回一个整数权值
 *   失败返回-1
 *  float powf(float b, float e);
 ****************************************************************************/
int cal_unfreed_count(struct task_mem_stats *tms)
{
  /** 分别对应未释放次数， 未释放内存块大小，以及最终的权值 */
  int unfreed_count, unfreed_size, ufc_val;
  int extra_weight_val = 0;

  if (!tms)
  {
    WARN("传入的tms为空!!!\n");
    return -1;
  }
  /** 未释放次数 * (10  +  2^(内存块大小 / 基准块大小)) */
  unfreed_count = tms->active_allocs;
  unfreed_size = tms->active_size;
  extra_weight_val = (int)powf((float)unfreed_size / MEMORY_ACTIVE_SIZE, 2.0);
  ufc_val = unfreed_count * (10 + extra_weight_val);

  INFO("-----------权值计算过程输出--------------\n");
  INFO("extra_weight_val:  %d\n", extra_weight_val);
  INFO("unfreed_count: %d\n", unfreed_count);
  INFO("unfreed_size:  %d\n", unfreed_size);
  INFO("计算公式: %d * (%d + 2^(%d / %d)) =%d", unfreed_count, WEIGHT_TABLE[WEIGHT_UNFREED_COUNT], unfreed_size, MEMORY_BLOCK_BASE, ufc_val);
  INFO("--------------------------------------\n");
  return ufc_val;
}

/****************************************************************************
 * Name: cal_unfreed_chunck_and_size
 *  ((Σ(活跃时间 * 活跃大小 ) ) / ( 标准内存警戒值 * 标准内存存活时间 * 未释放次数) ) * 30
 * Description:
 * Input Parameters:
 *  tms:  struct task_mem_stats *tms
 * Returned Value:
 *  return  an integer,  indicates  succeeding to updatate task_mem_statas or failing to update
 *  return -1 , ---> fail
 ****************************************************************************/
int cal_unfreed_chunck_and_size(struct task_mem_stats *tms)
{
  /** 获取内存的存活时间 * 内存的大小量 /  */
  /** 存活时间 * 大小 */
}
