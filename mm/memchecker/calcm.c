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
#define MAX_MM_UNFREED_COUNT (20)

/** 活跃内存大小 */
#define MAX_MM_ACTIVE_SIZE (1024)

volatile int dynamic_param = 12;

void report_err(LEAK_ERR err, pid_t pid)
{
  switch (err)
  {
  case TOO_MANY_UNFREED_ALLOC:
    syslog(LOG_INFO, "%s[task:%u] too Many unfreed allocs...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  case OVER_MAX_ACTIVE_SIZE:
    syslog(LOG_INFO, "%s[task:%u] active memory size is too much...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  case EXCESSIVE_SURVIVAL_TIME:
    syslog(LOG_INFO, "%s[task:%u] memory survives too long...%s\n",
           COLOR_TABLE[COLOR_RED], pid, COLOR_TABLE[COLOR_RESET]);
    break;
  default:
    syslog(LOG_INFO, "%s[task:%u] may have some memory leak problems...%s\n",
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
    syslog(LOG_INFO, "%srt_info is NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  if (!tms)
  {
    syslog(LOG_INFO, "%stms is NULL...\n%s",
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
  if (rt_info->max_mm_time > MAX_AGE_THRESHOLD)
  {
    report_err(EXCESSIVE_SURVIVAL_TIME, pid);
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
  const float mem_usage = ((float)rt_info->total_active_mm_size / (float)MAX_MM_ACTIVE_SIZE);

  /** 调节趋势权重 内存压力越大，趋势权重越高 */
  w->w_trend = 0.3 + 0.5 * mem_usage;

  INFO("total_active_mm_size:%d", rt_info->total_active_mm_size);
  INFO("MAX_MM_ACTIVE_SIZE:%d", MAX_MM_ACTIVE_SIZE);
  INFO("mem_usage:%.2f", mem_usage);

  /** 调节聚集度权重 未释放频率越高，聚集度权重越高 */
  /** 注意避免此时除0错误 */
  float alloc_freq = 0.0f;
  if (tms->total_alloc_count)
  {
    float alloc_freq = (float)rt_info->unfreed_count / (tms->total_alloc_count);
  }

  /** 若未释放频率为0 则不考虑增长部分的权重 */
  w->w_clustering = 0.2 + 0.3 * alloc_freq;
  INFO("unfreed_count:%d", rt_info->unfreed_count);
  INFO("total_alloc_count:%d", tms->total_alloc_count);
  INFO("w_clustering:%.2f", w->w_clustering);

  // 固定权重部分
  w->w_leak_rate = 0.4;
  /** 内存年龄 */
  w->w_age = 0.1;
  INFO("w_trend:%.2f  w_clustering: %.2f w_leak_rate:%.2f w_age:%.2f",
       w->w_trend, w->w_clustering, w->w_leak_rate, w->w_age);
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
    syslog(LOG_WARNING, "%srt_info can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  if (!tms)
  {
    syslog(LOG_WARNING, "%stms can't be NULL...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  if (!tms->total_alloc_count)
    return 0.0f;
  /*** 未释放率 */
  INFO("--------calc_leak_rate---------");
  INFO("unfreed_count:%d\n", rt_info->unfreed_count);
  INFO("total_alloc_count:%d\n", tms->total_alloc_count);
  INFO("Leak_Rate_Score: %.4f", (float)rt_info->unfreed_count / tms->total_alloc_count);
  INFO("-------------------------------");
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
    syslog(LOG_WARNING, "%srt_info can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }
  if (!tms)
  {
    syslog(LOG_WARNING, "%stms can't be NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }
  if ((tms->count < CHECKING_TIMES) || (tms->history_bytes.count < CHECKING_TIMES))
  {
    syslog(LOG_WARNING, "%scount < %u, can't calculate trend coeff...\n%s",
           COLOR_TABLE[COLOR_RED], CHECKING_TIMES, COLOR_TABLE[COLOR_RESET]);
    return 0.0f;
  }

  INFO("--------history_bytes--------");
  for (int i = 0; i < n; ++i)
  {
    sum_x += i;
    sum_y += hb_queue_get(&tms->history_bytes, i);
    INFO("history_bytes[%d]: %d",
         i, hb_queue_get(&tms->history_bytes, i));
    sum_xy += i * hb_queue_get(&tms->history_bytes, i);
    sum_xx += i * i;
  }
  INFO("-----------------------------");

  /** 通过最小二乘法进行计算 */
  numerator = n * sum_xy - sum_x * sum_y;
  denominator = n * sum_xx - sum_x * sum_x;

  /** 基本不存在分母为0的概率 */
  slope = (numerator) / (denominator);
  INFO("slope_score: %.4f", slope);
  return slope;
}

// 3. 分配聚集度（基于香农熵）
static float calc_clustering(struct task_mem_stats *tms)
{
  int i;
  uint32_t count[4] = {0}; // 统计四个时间段的分配次数
  const uint32_t window_len = tms->opq.count;
  float ret = 0.0f;

  INFO("--------operation_log------");
  for (i = 0; i < window_len; ++i)
  {
    // if (op_queue_get(&tms->opq, i) == ALLOC_LOG)
    // {
    //   INFO("operation[%d]:ALLOC_LOG", i);
    //   count[i % 4]++; // 将窗口分为4个时段
    // }
    switch (op_queue_get(&tms->opq, i))
    {
    case 0:
      INFO("ALLOC_LOG");
      count[i % 4]++;
      break;
    case 1:
      INFO("FREE_LOG");
      break;
    default:
      INFO("NONE");
      break;
    }
  }
  INFO("---------------------------\n");
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
  // 熵越低说明分配越集中 0.0014
  ret = 1.0f - (entropy / logf(4));
  INFO("clustering_score:%.4f", ret);
  return ret;
}

/****************************************************************************
 * Name: calc_age_score
 *  Description:
 *    根据内存驻留时间与最大驻留时间比例进行计算
 *    驻留时间越长, 越不安全
 * Input Parameters:
 *  struct rt_mem_info *rt_info
 *  struct task_mem_stats *tms
 * Returned Value:
 *        正常情况下返回对应浮点数结果
 *        否则对应返回0.0f
 ****************************************************************************/
static float calc_age_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  clock_t max_age;
  max_age = rt_info->max_mm_time;
  float ret = 0.0f;

  ret = ((float)max_age / MAX_AGE_THRESHOLD);

  INFO("+-------------------+-----------+");
  INFO("| %-16s | %9d |", "max_age:", max_age);
  INFO("| %-16s | %9d |", "MAX_AGE_THRESHOLD:", MAX_AGE_THRESHOLD);
  INFO("| %-16s | %9.4f |", "age_score:", ret);
  INFO("+-------------------+-----------+");
  return ret;
}

/** 可能还需要针对进程  */
static void print_leak_score(weight_factors_t *w, relevant_score_t *rs, struct task_mem_stats *tms)
{
  if (!w || !rs)
  {
    syslog(LOG_WARNING, "%s @w or @rs is NULL, which is not allowed...\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  if (!tms)
  {
    syslog(LOG_WARNING, "%s tms can't be NULL..\n%s",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return;
  }
  syslog(LOG_INFO,
         "%s"
         "┌──────────────────────────────┐\n"
         "│ pid:%-9d  count:%-4d    │\n"
         "├──────────────────────────────┤\n"
         "│ %-12s  %6s  %6s │\n"
         "├──────────────────────────────┤\n"
         "│ Leak Rate    %6.2f × %-6.2f │\n"
         "│ Trend Coeff  %6.2f × %-6.2f │\n"
         "│ Clustering   %6.2f × %-6.2f │\n"
         "│ Age          %6.2f × %-6.2f │\n"
         "├──────────────────────────────┤\n"
         "│ Final Score    %11.2f   │\n"
         "└──────────────────────────────┘%s",
         COLOR_TABLE[COLOR_MAGENTA],
         tms->pid, tms->count,
         "Metric", "Value", "Weight", // 表头
         rs->R, w->w_leak_rate,
         rs->T, w->w_trend,
         rs->C, w->w_clustering,
         rs->A, w->w_age,
         rs->FinalValue,
         COLOR_TABLE[COLOR_RESET]);
}

float calculate_leak_score(struct rt_mem_info *rt_info, struct task_mem_stats *tms)
{
  // 动态调整更新权重
  weight_factors_t w = {0};
  relevant_score_t rs = {0};
  update_weights(rt_info, &w, tms);

  // 计算各指标
  rs.R = calc_leak_rate(rt_info, tms);
  rs.T = calc_trend_coeff(rt_info, tms);
  rs.C = calc_clustering(tms);
  rs.A = calc_age_score(rt_info, tms);
  float final_value = 0.0f;

  // 权值相乘计算
  final_value = (rs.R * w.w_leak_rate) +
                (rs.T * w.w_trend) +
                (rs.C * w.w_clustering) +
                (rs.A * w.w_age);
  rs.FinalValue = final_value;
  print_leak_score(&w, &rs, tms);
  return final_value;
}
