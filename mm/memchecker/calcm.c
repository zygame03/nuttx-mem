/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include "calcm.h"
#include "mmdebug.h"
#include <syslog.h>
#include <stdio.h>
#include "utils.h"
#include <nuttx/list.h>
#include <nuttx/lib/math.h>

#define MEMORY_ACTIVE_SIZE (1024)
#define MEMORY_TIME_BASE 500
/** 内存的生存周期时间 20s  */
#define ACTIVE_TIME_PERIOD 20

/**  指定权值 */
static int WEIGHT_TABLE[WEIGHT_NUM] = {
    [WEIGHT_ACTIVE_ALLOCS] = 8,
    [WEIGHT_CHUNCK_AND_SIZE] = 30,
    [WEIGHT_AGE] = 10};

void print_leak_err_info(enum LEAK_ERR err)
{
  switch (err)
  {
  case UNFREEED_NUM:
    syslog(LOG_INFO, "%s Excessive amount of unreleased memory!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case UNFREEED_CHUNK:
    syslog(LOG_INFO, "%s Too much unfreed memory!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case HIGH_GROWTH_RATE:
    syslog(LOG_INFO, "%s内存申请增速过快!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case LEAK:
    syslog(LOG_INFO, "%smemory leaks!!!%s\n", COLOR_TABLE[COLOR_RE
  }
}

int is_basic_err(struct task_mem_stats *tms)
{
  /** 此时表示一定存在有未释放的内存 */
  if (!tms->active_allocs && tms->active_size)
  {
    print_leak_err_info(LEAK);
    syslog(LOG_WARNING, " %s There is memory leak...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 返回0 表示不存在基础性的问题 */
  return 0;
}

// 获取pid的内存信息
static void get_mem_info(struct task_mem_stats *tms, struct mem_info *info)
{
  clock_t now_time = clock_systime_ticks();

  struct memchecker_metadata *metadata = NULL;

  list_for_every_entry(&tms->metadata_list, metadata, struct memchecker_metadata, node_for_ld)
  {
    info->total_count++;
    if (metadata->state == MEMCHECKER_ALLOCATED)
    {
      info->unfreed_count++;

      info->total_size += metadata->size;
      if (metadata->size > info->max_size)
      {
        info->max_size = metadata->size;
      }

      info->total_time += now_time - metadata->alloc_track.ts;
      if (now_time - metadata->alloc_track.ts > info->max_time)
      {
        info->max_time = now_time - metadata->alloc_track.ts;
      }
    }
    else if (metadata->state == MEMCHECKER_FREED)
    {
      info->total_time +=
          metadata->free_track.ts - metadata->alloc_track.ts;
      if (metadata->free_track.ts - metadata->alloc_track.ts >
          info->max_time)
      {
        info->max_time =
            metadata->free_track.ts - metadata->alloc_track.ts;
      }
    }
  }

  // 此处是基础分值的赋值
  info->size_score = 1;
  info->count_score = 5;
  info->time_score = 1;

  return;
}

int weight_cal(struct task_mem_stats *tms)
{
  struct mem_info info = {0};
  get_mem_info(tms, &info);

  info.size_score *= info.max_time / 1000;
  info.size_score *= info.unfreed_count / info.total_count;

  tms->individual_score[0] = info.size_score * info.total_size;

  tms->individual_score[1] = info.count_score * info.unfreed_count;

  info.time_score *= info.total_time / 100;

  tms->individual_score[2] = info.time_score * info.max_time;

  double score = 0;
  for (int i = 0; i < 3; i++)
  {
    score += tms->individual_score[i] * tms->ratio[i];
  }
  tms->score = score;

  return score;
}