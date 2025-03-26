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

/****************************************************************************
 *  以下宏为基本检查的标准值
 *         触发条件一般来说较为苛刻
 ****************************************************************************/
/** 假设内存块最大时间为一分钟 */
#define MAX_AGE_THRESHOLD (6000)

/** 最大活跃内存块数 */
#define MAX_MM_UNFREED_COUNT (15)

/** 内存块总大小 */
#define MAX_MM_ACTIVE_SIZE (2048)

volatile int dynamic_param = 12;
void report_err(LEAK_ERR err, pid_t pid)
{
  switch (err)
  {
  case TOO_MANY_UNFREED_ALLOC:
    syslog(LOG_INFO, "%s [task:%u]-Too Many unfreed allocs...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  case OVER_MAX_ACTIVE_SIZE:
    syslog(LOG_INFO, "%s [task:%u]-Active memory size is too much...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  case EXCESSIVE_SURVIVAL_TIME:
    syslog(LOG_INFO, "%s [task:%u] might have memory not freed...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  default:
    syslog(LOG_INFO, "%s [task:%u] might have some memory leak problems...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  }
}

/****************************************************************************
 *  初级检测逻辑 ,只涉及一些基本的检测比较逻辑
 *      若满足会造成报警, 但触发条件其实较为苛刻
 *      主要作用是过度到后续复杂权值计算
 ****************************************************************************/
int basic_meomory_leak_check(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  if (!rt_info)
  {
    syslog(LOG_INFO, "%s rt_info is NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (!tms)
  {
    syslog(LOG_INFO, "%s tms is NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  pid_t pid = tms->pid;
  /** 默认不会有错 */
  bool isErr = false;

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
  /**--- 内存块活跃时间过长 --- */
  if (rt_info->max_mm_time > EXCESSIVE_SURVIVAL_TIME)
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
  const float mem_usage = (float)rt_info->total_active_mm_size / MAX_MM_ACTIVE_SIZE;

  /** 调节趋势权重 内存压力越大，趋势权重越高 */
  w->w_trend = 0.3 + 0.5 * mem_usage;

  /** 调节聚集度权重 未释放频率越高，聚集度权重越高 */
  float alloc_freq = (float)rt_info->unfreed_count / (tms->total_alloc_count);
  w->w_clustering = 0.2 + 0.3 * alloc_freq;

  // 固定权重部分
  w->w_leak_rate = 0.4;
  /** 内存年龄 */
  w->w_age = 0.1;
}

/****************************************************************************
 * Name:calc_leak_rate
 *  Description:
 *        计算由未释放内存的占比
 * Input Parameters:
 *  struct rt_mem_info *rt_info
 *  struct task_mem_stats *tms
 * Returned Value:
 *      return 0.0f 可能存在某些问题
 *       else return 驻留内存占比比率
 ****************************************************************************/
float calc_leak_rate(const struct rt_mem_info *rt_info, const struct task_mem_stats *tms)
{
  if (!rt_info)
  {
    syslog(LOG_WARNING, "%s rt_info can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  if (!tms->total_alloc_count)
    return 0.0f;
  /*** 未释放率 */
  INFO("unfreed_count:%d\n", rt_info->unfreed_count);
  INFO("total_alloc_count:%d\n", tms->total_alloc_count);
  INFO("leak_rate: %.2f", (float)rt_info->unfreed_count / tms->total_alloc_count);
  return (float)(rt_info->unfreed_count) / tms->total_alloc_count;
}

/****************************************************************************
 * Name: calc_trend_coeff
 *  Description:
 *      根据驻留内存的增长速度增长权值计算
 * Input Parameters:
 *  struct rt_mem_info *rt_info
 *  struct task_mem_stats *tms
 * Returned Value:
 *      正常情况下, 计算得出对应拟合增长曲线的斜率
 *        否则返回0.0f
 * todo  检查一下这里的未释放率的是否合理
 ****************************************************************************/
static float calc_trend_coeff(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
  float numerator, denominator, slope;
  const int n = tms->history_bytes.count;

  if (!rt_info)
  {
    syslog(LOG_WARNING, "%s rt_info can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }
  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  if ((tms->count < CHECKING_TIMES) || (tms->history_bytes.count < CHECKING_TIMES))
  {
    syslog(LOG_WARNING, "%s count < %u, can't calculate trend coeff...\n%s",
           COLOR_TABLE[COLOR_RED], CHECKING_TIMES, COLOR_TABLE[COLOR_RESET]);
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
  uint32_t count[3] = {0}; // 统计四个时间段的分配次数
  const uint32_t window_len = tms->opq.count;

  for (uint32_t i = 0; i < window_len; ++i)
  {
    if (tms->opq.buffer[tms->opq.head + 1] == ALLOC_LOG)
    {
      count[i % 3]++; // 将窗口分为4个时段
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

// 4. 内存年龄评分  /*** 超过一分钟 > 30s  */
static float calc_age_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  uint32_t current_tick = clock_systime_ticks();
  float max_age = 0.0f;
  max_age = rt_info->max_mm_time;

  return max_age / MAX_AGE_THRESHOLD;
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
  float final_vaule = 0.0f;
  syslog(LOG_INFO, "%sR:%.2f T:%.2f C:%.2f A:%.2f \n%s",
         COLOR_TABLE[COLOR_GREEN], R, T, C, A, COLOR_TABLE[COLOR_RESET]);
  // 综合评分
  final_vaule = (R * w.w_leak_rate) +
                (T * w.w_trend) +
                (C * w.w_clustering) +
                (A * w.w_age);
  return final_vaule;
}