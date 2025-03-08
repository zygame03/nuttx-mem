/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/mm/memchecker.h>
#include <nuttx/mm/calcm.h>
#include <nuttx/mm/mmdebug.h>

/****************************************************************************
 * calc_W1 --- 平均计算时间
 * avg_age_ticks ---  节拍
 * tick_rate   --- 系统时钟频率
 * 计算公式:
 *  avg_age = Σ(current_time - alloc_ts)/unreleased_count + 1
 *  W1 = log2(avg_age / 基准时间)
 ****************************************************************************/
/**
 * @brief 计算存活时间权重
 * @param proc 进程元数据指针
 * @param now_ticks 当前系统tick数
 * @param tick_rate 每秒tick数（从CLOCKS_PER_SEC获取）
 * @return W1权重值（0~2）
 */
/***
 *
 * Todo: 平均时长获取(pid  === > + = 内存驻留时间(ticks) === > 时间(s)   )
 *
 *
 */
float calc_W1(struct task_mem_stats *tms, uint32_t now_ticks, uint32_t tick_rate)
{
  // if (list_empty(&proc->unreleased_list))
  //   return 0.0f;
  /** 如果当前进程全部都已经释放则权重返回为0 */
  if (!tms->active_allocs)
  {
    return 0.0f;
  }
  // // 计算未释放块的平均存活时间（秒）
  // float total_age = 0.0f;
  // struct mem_meta *pos;
  // list_for_each_entry(pos, &proc->unreleased_list, list)
  // {
  //   total_age += (now_ticks - pos->timestamp) / (float)tick_rate;
  // }
  // float avg_age = total_age / list_length(&proc->unreleased_list);
  float total_ticks_age = 0.0f;
  total_ticks_age = get_all_age(tms->pid); // Todo：通过号获得，节拍数和;
                                           // 节拍数处理得到秒数
  // 通过以下计算返回权值
  // // 对数增长权重：log2(avg_age/10 + 1)
  // return log2f(avg_age / 10.0f + 1.0f);
  // 通过log2f 返回权值
}

/**
 * @brief 计算内存失衡权重
 * @param proc 进程元数据指针
 * @return W2权重值（0~1）
 */
float calc_W2(struct task_mem_stats *tms)
{
  // 动态计算申请/释放比
  float ratio = (tms->total_allocs - tms->active_allocs) / (tms->total_allocs + 1.0f);

  // Sigmoid激活函数强化特征
  return 1.0f / (1.0f + expf(-5.0f * (ratio - 0.3f)));
}

/** 这里待检测 */
float calc_W3(struct task_mem_stats *tms, float check_interval_sec, float mem_guard_line)
{
  if (check_interval_sec <= 0)
    return 0.0f;

  // 计算单位时间内存增长量（MB/s）
  float mem_growth = (proc->current_mem_mb - proc->last_mem_mb) / check_interval_sec;

  // 双曲正切归一化
  return tanhf(mem_growth / mem_guard_line);
}