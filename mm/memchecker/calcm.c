/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include "calcm.h"
#include "mmdebug.h"
#include <syslog.h>
#include <stdio.h>
#include "utils.h"
#include <nuttx/lib/math.h>

#define MEMORY_ACTIVE_SIZE (1024)
#define MEMORY_TIME_BASE 500
/** 内存的生存周期时间 20s  */
#define ACTIVE_TIME_PERIOD 20

enum WEIGHT
{
  WEIGHT_ACTIVE_ALLOCS,
  WEIGHT_CHUNCK_AND_SIZE,
  WEIGHT_AGE,
  WEIGHT_NUM
};

/**  指定权值 */
static int WEIGHT_TABLE[WEIGHT_NUM] = {
    [WEIGHT_ACTIVE_ALLOCS] = 8,
    [WEIGHT_CHUNCK_AND_SIZE] = 30,
    [WEIGHT_AGE] = 10};

// 全局参数 测试外部调试
volatile int dynamic_param = 8;

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
    syslog(LOG_INFO, "%s Excessive amount of unreleased memory!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case UNFREEED_CHUNK:
    syslog(LOG_INFO, "%s Too much unfreed memory!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case HIGH_GROWTH_RATE:
    syslog(LOG_INFO, "%s内存申请增速过快!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
  case LEAK:
    syslog(LOG_INFO, "%smemory leaks!!!%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    break;
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

/****************************************************************************
 * Name: cal_w1()
 * Description:
 *   计算方式:
 *    额外权值: 活跃内存 / 活跃的基准警戒大小
 *    未释放次数 * (10  + e^ (额外权值) )
 *    由于可能由于活跃内存的剧烈增长所造成的'指数级爆炸增长'
 *    我们将此得分的最高分值限制在60分以内
 *  待暴漏的值:  活跃
 * Input Parameters:
 *   tms -  struct task_mem_stats *tms
 *
 * Returned Value:
 *   成功, 返回一个整数权值
 *   失败返回-1
 *  float powf(float b, float e);
 ****************************************************************************/
float cal_w1(struct task_mem_stats *tms)
{
  /** 分别对应未释放次数， 未释放内存块大小，以及最终的权值 */
  int active_allocs, active_size;
  float extra_weight_val = 0.0, w1_val;

  syslog(LOG_INFO, "dynamic_parm:%d ...\n", dynamic_param);

  if (!tms)
  {
    syslog(LOG_WARNING, "%s @tms is NULL...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /** 未释放次数 * (10  +  e^(内存块大小 / 基准块大小)) */
  active_allocs = tms->active_allocs;
  active_size = tms->active_size;
  extra_weight_val = powf((float)active_size / MEMORY_ACTIVE_SIZE, M_E);
  w1_val = (active_allocs * (10 + extra_weight_val));

  syslog(LOG_INFO,
         "%s\n▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖ Calculating Process▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖\n"
         "  |- e^(active_size / standard_memory_size ):   %.2f\n"
         "  |- active_allocs:   %d\n"
         "  |- active_size:     %d(B)\n"
         "  |- formula:         active_allocs * (%d + %.3f^(extra_val))\n"
         "  |- process:         %d * (%d + 2^(%d / %d))\n"
         "  |- result:          %.2f\n"
         "▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖\
         %s",
         COLOR_TABLE[COLOR_BLUE],
         extra_weight_val,
         active_allocs,
         active_size,
         WEIGHT_TABLE[WEIGHT_ACTIVE_ALLOCS],
         M_E,
         active_allocs,
         WEIGHT_TABLE[WEIGHT_ACTIVE_ALLOCS],
         active_size,
         MEMORY_ACTIVE_SIZE,
         w1_val,
         COLOR_TABLE[COLOR_RESET]);
  return w1_val;
}

/****************************************************************************
 * Name: cal_w2()
 *    计算方式:
 *          内存块大小 与 权值关系:  内存大小因子
 *                 0 ~ 1/10     ===> 1
 *              1/10 ~ 1/5      ===> 2
 *              ```````
 *              9/10 ~ 1        ===> 9
 *              超过了内存警戒值:
 *              则  按照倍数(保留小数) * 10
 *        存活时间系数 =  log2( 1 + (存活时间/基准时间))
 *        ∑(内存块大小参数值 × 存活时间系数) 内存块
 * Description:
 * Input Parameters:
 *  tms:  struct task_mem_stats *tms
 * Returned Value: 成功则返回计算好的权值
 *    失败返回-1
 ****************************************************************************/
static int get_active_mem_factor(int memory_size)
{
  float memory_arg = (float)memory_size;
  int factor;
  float times;

  if (0 > memory_size)
  {
    syslog(LOG_WARNING, "%s memory_size为负数, 传参出现严重问题...%s\n", COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }
  /**低于活跃内存警戒线 */
  if (isgreater(MEMORY_ACTIVE_SIZE, memory_arg))
  {
    if (isgreater(memory_arg, 0.9 * MEMORY_ACTIVE_SIZE))
    {
      factor = 10;
    }
    else /** 此时低于0.9 */
    {
      if (isgreater(memory_arg, 0.8 * MEMORY_ACTIVE_SIZE))
      {
        factor = 9;
      }
      else
      {
        if (isgreater(memory_arg, 0.7 * MEMORY_ACTIVE_SIZE))
        {
          factor = 8;
        }
        else
        {
          if (isgreater(memory_arg, 0.6 * MEMORY_ACTIVE_SIZE))
          {
            factor = 7;
          }
          else
          {
            if (isgreater(memory_arg, 0.5 * MEMORY_ACTIVE_SIZE))
            {
              factor = 6;
            }
            else
            {
              if (isgreater(memory_arg, 0.4 * MEMORY_ACTIVE_SIZE))
              {
                factor = 5;
              }
              else
              {
                if (isgreater(memory_arg, 0.3 * MEMORY_ACTIVE_SIZE))
                {
                  factor = 4;
                }
                else
                {
                  if (isgreater(memory_arg, 0.2 * MEMORY_ACTIVE_SIZE))
                  {
                    factor = 3;
                  }
                  else
                  {
                    if (isgreater(memory_arg, 0.1 * MEMORY_ACTIVE_SIZE))
                    {
                      factor = 2;
                    }
                    else /** 预警内存的 1/10 都不到 */
                    {
                      factor = 1;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  else
  {
    times = memory_arg / MEMORY_ACTIVE_SIZE;
    factor = (int)(times * 10);
  }
  return factor;
}

float cal_w2(struct task_mem_stats *tms)
{
  /** 通过tms  进入循环遍历 每个结构体 */
  struct memchecker_metadata *buffer[CONFIG_MM_MEMCHECKER_PAGE_NUMBER] = {0};
  int count, i, factor, w2_value;
  uint32_t ts, cur_ts;
  int active_time;
  float active_time_ratio, sum_value = 0.0;

  count = pid_to_metadata(tms->pid, &buffer);
  if (0 >= count)
  {
    syslog(LOG_WARNING, "%sTask doesn't exit or some other problems...%s\n",
           COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
    return -1;
  }

  for (i = 0; i < count; i++)
  {
    factor = get_active_mem_factor(buffer[i]->size);
    cur_ts = clock_systime_ticks();
    /** 这里的500对应频率  */
    active_time = (cur_ts - buffer[i]->alloc_track.ts) / MEMORY_TIME_BASE;
    active_time_ratio = log2f(1 + active_time);

    if (0 > factor)
    {
      syslog(LOG_WARNING, "%sfactor can't be a negative number...%s\n",
             COLOR_TABLE[COLOR_RED], COLOR_TABLE[COLOR_RESET]);
      return;
    }
    sum_value += factor * active_time_ratio;
  }
  return (int)sum_value;
}

/****************************************************************************
 * write by zy
 ****************************************************************************/

struct task_mem_info
{
  struct list_node node;
  pid_t pid;
  uint32_t total_size;
  uint32_t unfreed_num;
  clock_t active_time_avg;
  clock_t active_time_max;
  clock_t active_time_min;
};

static struct list_node task_list;

static double size_base_score = 1;   // 内存大小基准分数倍率
static double count_base_score = 10; // 未释放数量基准分数倍率
static double time_base_score = 1;   // 存活时间基准分数倍率

static double size_weight = 0.4;  // 大小权重系数
static double count_weight = 0.4; // 未释放数量权重系数
static double time_weight = 0.2;  // 存活时间权重系数

// 通过pid获取task_mem_info
struct task_mem_info *pid_to_task_mem_info(pid_t pid)
{
  struct task_mem_info *info;
  list_for_every_entry(&task_list, info, struct task_mem_info, node)
  {
    if (info->pid == pid)
    {
      return info;
    }
  }
  return NULL;
}

// 获取pid的内存信息
void get_weights(pid_t pid)
{
  struct memchecker_metadata *metadata_list[CONFIG_MM_MEMCHECKER_PAGE_NUMBER];

  int count = pid_to_metadata(pid, metadata_list);
  if (count <= 0)
  {
    return;
  }

  // 获取pid对应的task_mem_info，没有则创建
  struct task_mem_info *info = pid_to_task_mem_info(pid);
  if (info == NULL)
  {
    struct task_mem_info *new_info =
        (struct task_mem_info *)malloc(sizeof(struct task_mem_info));
    new_info->pid = pid;
    list_initialize(&new_info->node);
    list_add_tail(&task_list, &new_info->node);
    info = new_info;
  }

  info->unfreed_num = count;

  clock_t now_time = clock_systime_ticks();
  clock_t total_time = 0;
  size_t total_size = 0;
  struct memchecker_metadata *metadata = NULL;

  for (int i = 0; i < count; i++)
  {
    metadata = metadata_list[i];
    if (metadata->state == MEMCHECKER_ALLOCATED)
    {
      total_size += metadata->size;

      clock_t active_time = now_time - metadata->alloc_track.ts;
      total_time += active_time;

      // 找最长和最短存活时间
      if (active_time > info->active_time_max)
      {
        info->active_time_max = active_time;
      }
      if (info->active_time_min == 0 ||
          active_time < info->active_time_min)
      {
        info->active_time_min = active_time;
      }
    }
  }

  info->total_size = total_size;

  if (total_time > 0)
  {
    info->active_time_avg = total_time / count;
  }
}

int weight_cal(struct task_mem_info *info)
{
  double size_score = size_base_score;
  double count_score = count_base_score;
  double time_score = time_base_score;

  switch (TICK2SEC(info->active_time_avg) / 10)
  {
  case 0:
  case 1:
  case 2:
  {
    size_score *= 1;
    break;
  }
  case 3:
  case 4:
  case 5:
  {
    size_score *= 1.1;
    break;
  }
  case 6:
  case 7:
  case 8:
  {
    size_score *= 1.25;
    break;
  }
  default:
  {
    size_score *= 1.5;
    break;
  }
  }

  size_score *= (double)info->total_size;
  if (size_score > 100)
  {
    size_score = 100;
  }

  count_score *= (double)info->unfreed_num;
  if (count_score > 100)
  {
    count_score = 100;
  }

  time_score *= (double)info->active_time_max;
  if (time_score > 100)
  {
    time_score = 100;
  }

  double weight = size_weight * size_score +
                  count_weight * count_score +
                  time_weight * time_score;

  return (int)weight;
}