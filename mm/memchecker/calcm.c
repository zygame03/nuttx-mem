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

volatile int dynamic_param = 12;
void report_err(LEAK_ERR err, pid_t pid)
{
  switch (err)
  {
  case TOO_MANY_UNFREED_ALLOC:
    syslog(LOG_INFO, "%s [task:%u] Too Many unfreed allocs...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  case OVER_MAX_ACTIVE_SIZE:
    syslog(LOG_INFO, "%s [task:%u] Active memory size is too much...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  default:
    syslog(LOG_INFO, "%s [task:%u] might have some meomory leak problems...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  }
}

/*** 此处只是针对元数据做一些基本的跟踪 如果出现问题， 则此时  生成一个最近的分析日志 */
int basic_meomory_leak_check(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  pid_t pid = tms->pid;
  bool isErr = false;

  if (!rt_info || !tms)
  {
    syslog(LOG_INFO, "%s rt_info or tms is NULL, which is not allowed...%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  /** --- 活跃块数过多  --- */
  if (rt_info->unfreed_count > MAX_MM_UNFREED_COUNT)
  {
    report_err(TOO_MANY_UNFREED_ALLOC, pid);
    isErr = true;
  }
  /** --- 活跃内存超出正常范围 --- */
  if (rt_info->max_mm_size > MAX_MM_ACTIVE_SIZE)
  {
    report_err(OVER_MAX_ACTIVE_SIZE, pid);
    isErr = true;
  }
  if (isErr)
    return 1;
  else
    return 0;
}

//  动态调整部分权重值
static void update_weights(struct rt_mem_info *rt_info, weight_factors_t *w, struct task_mem_stats *tms)
{
  const float mem_usage = (float)rt_info->total_active_mm_size / MAX_MEMORY;

  /**  内存压力越大，趋势权重越高 */
  w->w_trend = 0.3 + 0.5 * mem_usage;

  // 未释放频率越高，聚集度权重越高
  float alloc_freq = (float)rt_info->unfreed_count / (tms->total_alloc_count);
  w->w_clustering = 0.2 + 0.3 * alloc_freq;

  // 固定权重部分
  w->w_leak_rate = 0.4;
  /** 内存年龄 */
  w->w_age = 0.1;
}

/** 关键指标计算 */
// 1. 未释放率（0~1） 50 100 毫无疑问 是可以把tms 传输进来的
float calc_leak_rate(const struct rt_mem_info *rt_info, const struct task_mem_stats *tms)
{
  if (!tms->total_alloc_count)
    return 0.0f;
  /*** 未释放率 */
  INFO("unfreed_count:%d\n", rt_info->unfreed_count);
  INFO("total_alloc_count:%d\n", tms->total_alloc_count);
  return (float)(rt_info->unfreed_count) / tms->total_alloc_count;
}

// 进行60s的检查
// 2. 内存增长趋势（使用线性回归）  高于5次检查以后*//
static float calc_trend_coeff(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
  float numerator, denominator, slope;
  const int n = tms->history_bytes.count;

  if ((tms->count < CHECKING_TIMES) || (tms->history_bytes.count < CHECKING_TIMES))
  {
    syslog(LOG_WARNING, "%s count < 60, can't calculate trend coeff...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  for (int i = 0; i < n; ++i)
  {
    sum_x += i;
    sum_y += hb_queue_get(&tms->history_bytes, i);
    INFO("history_bytes[%d]: %d\n",
         i, hb_queue_get(&tms->history_bytes, i));
    sum_xy += i * hb_queue_get(&tms->history_bytes, i);
    sum_xx += i * i;
  }

  /** 最小二乘法 */
  numerator = n * sum_xy - sum_x * sum_y;
  denominator = n * sum_xx - sum_x * sum_x;
  /* 如果说分母为0 */
  /** 一般来讲 不会分母不会为0*/
  slope = (numerator) / (denominator);
  // mon->trend_coeff = slope;
  return slope;
}

// 3. 分配聚集度（基于香农熵） 暂时无法计算
static float calc_clustering(struct task_mem_stats *tms)
{
  uint32_t count[4] = {0}; // 统计四个时间段的分配次数
  const uint32_t window_len = tms->opq.count;

  for (uint32_t i = 0; i < window_len; ++i)
  {
    if (tms->opq.buffer[tms->opq.head + 1] == ALLOC_LOG)
    {
      count[i % 4]++; // 将窗口分为4个时段
    }
  }
  // 计算熵值
  float entropy = 0.0f;
  for (int j = 0; j < 4; ++j)
  {
    if (count[j] > 0)
    {
      float p = (float)count[j] / window_len;
      entropy -= p * logf(p);
    }
  }
  // 熵越低说明分配越集中
  return 1.0f - (entropy / logf(4));
}

// 4. 内存年龄评分  /*** 超过一分钟 > 30s  */
static float calc_age_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  uint32_t current_tick = clock_systime_ticks();
  float max_age = 0.0f;
  max_age = rt_info->max_mm_time;

  return max_age / MAX_AGE_THRESHOLD; // 超过阈值则得1分
}

/**  */
float calculate_leak_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  // 更新权重
  weight_factors_t w;
  update_weights(rt_info, &w, tms);

  // 计算各指标
  const float R = calc_leak_rate(rt_info, tms);
  const float T = calc_trend_coeff(rt_info, tms);
  const float C = calc_clustering(tms);
  const float A = calc_age_score(rt_info, tms);

  syslog(LOG_INFO, "%sR:%.2f T:%.2f C:%.2f A:%.2f \n%s",
         COLOR_TABLE[COLOR_GREEN], R, T, C, A, COLOR_TABLE[COLOR_RESET]);
  // 综合评分
  return (R * w.w_leak_rate) +
         (T * w.w_trend) +
         (C * w.w_clustering) +
         (A * w.w_age);
}
