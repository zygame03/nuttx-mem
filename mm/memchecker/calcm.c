/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include "calcm.h"
#include "mmdebug.h"
#include <syslog.h>
#include "utils.h"
#include <nuttx/list.h>
#include <nuttx/lib/math.h>

#define MEMORY_ACTIVE_SIZE (1024)
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
 * Name: cal_W1
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
int cal_W1(struct task_mem_stats *tms)
{
  /** 分别对应未释放次数， 未释放内存块大小，以及最终的权值 */
  int unfreed_count, unfreed_size, ufc_val;
  float extra_weight_val = 0;

  if (!tms)
  {
    WARN("传入的tms为空!!!\n");
    return -1;
  }
  /** 未释放次数 * (10  +  2^(内存块大小 / 基准块大小)) */
  unfreed_count = tms->active_allocs;
  unfreed_size = tms->active_size;
  extra_weight_val = powf((float)unfreed_size / MEMORY_ACTIVE_SIZE, 2.0);
  ufc_val = (int)(unfreed_count * (10 + extra_weight_val));

  syslog(LOG_INFO,
         "%s\n\
          ▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖ Calculating Process▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖\n"
         "  |- 额外权重值:   %.2f\n"
         "  |- 未释放计数:   %d\n"
         "  |- 未释放大小:   %d\n"
         "  |- 计算公式:     未释放次数 * (基本权值(默认:10) + 2^(额外权重))\n"
         "  |- 计算过程:     %d * (%d + 2^(%d / %d))\n"
         "  |- 计算结果:     %d\n"
         "▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▗▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▖\
         %s",
         COLOR_TABLE[COLOR_RED],
         extra_weight_val,
         unfreed_count,
         unfreed_size,
         unfreed_count,
         WEIGHT_TABLE[WEIGHT_UNFREED_COUNT],
         unfreed_size,
         MEMORY_ACTIVE_SIZE,
         ufc_val,
         COLOR_TABLE[COLOR_RESET]);
  return ufc_val;
}

/****************************************************************************
 * Name: cal_W2
 *    计算方式:
 *          内存块大小 与 权值关系:  内存大小因子
 *                 0 ~ 1/10     ===> 1
 *              1/10 ~ 1/5      ===> 2
 *              ```````
 *              9/10 ~ 1        ===> 9
 *              超过了内存警戒值 ：
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

int cal_unfreed_chunck_and_size(struct task_mem_stats *tms)
{
}

/****************************************************************************
 * write by zy
 ****************************************************************************/

static struct list_node task_list;

static double size_base_score = 1;   // 内存大小基准分数倍率
static double count_base_score = 10;  // 未释放数量基准分数倍率
static double time_base_score = 1;   // 存活时间基准分数倍率

static double size_weight = 0.4;     // 大小权重系数
static double count_weight = 0.4;    // 未释放数量权重系数
static double time_weight = 0.2;     // 存活时间权重系数

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
  if(count <= 0)
    {
      return;
    }

  //获取pid对应的task_mem_info，没有则创建
  struct task_mem_info *info = pid_to_task_mem_info(pid);
  if(info == NULL)
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

  for(int i = 0; i < count; i++) 
    {
      metadata = metadata_list[i];
      if(metadata->state == MEMCHECKER_ALLOCATED)
        {
          total_size += metadata->size;

          clock_t active_time = now_time - metadata->alloc_track.ts;
          total_time += active_time;
          
          // 找最长和最短存活时间
          if(active_time > info->active_time_max)
            {
              info->active_time_max = active_time;
            }
          if(info->active_time_min == 0 || 
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

  switch(TICK2SEC(info->active_time_avg) / 10)
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
  if(size_score > 100)
    {
      size_score = 100;
    }
  
  count_score *= (double)info->unfreed_num;
  if(count_score > 100)
    {
      count_score = 100;
    }

  time_score *= (double)info->active_time_max;
  if(time_score > 100)
    {
      time_score = 100;
    }

  double weight = size_weight * size_score + 
                  count_weight * count_score + 
                  time_weight * time_score;

  return (int)weight;
}
