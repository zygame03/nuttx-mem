/****************************************************************************
 * include/nuttx/mm/calcm.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_MM_CALCM_H
#define __INCLUDE_NUTTX_MM_CALCM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <stdint.h>

#include <nuttx/mm/leakdetector.h>
#include <nuttx/lib/math.h>

/****************************************************************************
 * 使用Q15格式（16位整数表示[-1,1)范围
 *  基准时间10秒的小数）
 *  先使用float角度去写
 ****************************************************************************/

#define Q15_SHIFT 15
#define BASE_TIME_SEC 10

float calc_W1(struct task_mem_stats *tms, uint32_t now_ticks, uint32_t tick_rate);

/**
 * @brief 计算内存失衡权重
 * @param proc 进程元数据指针
 * @return W2权重值（0~1）
 */
float calc_W2(struct task_mem_stats *tms);

/**
 * @brief 计算内存增长权重
 * @param proc 进程元数据指针
 * @param check_interval_sec 检测间隔（秒）
 * @param mem_guard_line 内存警戒线（MB）
 * @return W3权重值（0~1）
 */
float calc_W3(struct task_mem_stats *tms, float check_interval_sec, float mem_guard_line);

/**
 * @brief 计算调用栈聚集权重
 * @param proc 进程元数据指针
 * @return W4权重值（0~0.5）
 */
// float calc_W4(struct proc_leak_meta *proc)
// {
//   if (list_empty(&proc->unreleased_list))
//     return 0.0f;

//   // 统计最常出现的调用栈模式
//   uint32_t hash_counts[32] = {0};
//   uint32_t max_count = 0;

//   struct mem_meta *pos;
//   list_for_each_entry(pos, &proc->unreleased_list, list)
//   {
//     int idx = pos->call_stack_hash % 32;
//     hash_counts[idx]++;
//     if (hash_counts[idx] > max_count)
//     {
//       max_count = hash_counts[idx];
//     }
//   }

//   // 计算最大聚集度
//   return (float)max_count / list_length(&proc->unreleased_list);
// }

#endif /* __INCLUDE_NUTTX_MM_GRAN_H */