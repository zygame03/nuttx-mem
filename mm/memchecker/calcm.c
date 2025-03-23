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
    syslog(LOG_INFO, "%smemory leaks!!!%s\n", COLOR_TABLE[COLOR_BLUE], COLOR_TABLE[COLOR_RESET]);
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

#define MAX_MEMORY 1024

// 根据系统状态调整权重 设置一套基准值
// 基本值
static void update_weights(struct rt_mem_info *rt_info, weight_factors_t *w)
{

  const float mem_usage = (float)rt_info->total_active_mm_size / MAX_MEMORY;

  /**  内存压力越大，趋势权重越高 */
  w->w_trend = 0.3 + 0.5 * mem_usage;

  // 分配频率越高，聚集度权重越高
  float alloc_freq = (float)rt_info->unfreed_count / (rt_info->total_alloc_count);
  w->w_clustering = 0.2 + 0.3 * alloc_freq;

  // 固定权重部分
  w->w_leak_rate = 0.6;
  /** 内存年龄 */
  w->w_age = 0.2;
}

/** 关键指标计算 */
// 1. 未释放率（0~1）
float calc_leak_rate(const struct rt_mem_info *rt_info)
{
  if (rt_info->total_alloc_count == 0)
    return 0.0f;
  return (float)(rt_info->unfreed_count) / rt_info->total_alloc_count;
}

// 2. 内存增长趋势（使用线性回归）  高于20次检查以后*//
float calc_trend_coeff(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
  const int n = tms->history_bytes.count;

  for (int i = 0; i < n; ++i)
  {
    sum_x += i;
    sum_y += queue_get(&tms->history_bytes, i);
    sum_xy += i * queue_get(&tms->history_bytes, i);
    sum_xx += i * i;
  }

  /** 最小二乘法 */
  const float slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);
  // mon->trend_coeff = slope;
  return slope;
}

// 3. 分配聚集度（基于香农熵）
float calc_clustering(struct task_mem_stats *tms)
{
  uint32_t count[3] = {0}; // 统计四个时间段的分配次数
  const uint32_t window_len = tms->opq.count;

  for (uint32_t i = 0; i < window_len; ++i)
  {
    if (tms->opq.buffer[tms->opq.head + 1] == ALLOC_LOG)
    {
      count[i % 3]++; // 将窗口分为3个时段
    }
  }
  // 计算熵值
  float entropy = 0.0f;
  for (int j = 0; j < 3; ++j)
  {
    if (count[j] > 0)
    {
      float p = (float)count[j] / window_len;
      entropy -= p * logf(p);
    }
  }
  // 熵越低说明分配越集中
  return 1.0f - (entropy / logf(3));
}

// 4. 内存年龄评分  /*** 超过一分钟 > 1 */
#define MAX_AGE_THRESHOLD 60000
float calc_age_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  uint32_t current_tick = get_system_tick();
  float max_age = 0.0f;
  max_age = rt_info->max_mm_time;

  return max_age / MAX_AGE_THRESHOLD; // 超过阈值则得1分
}