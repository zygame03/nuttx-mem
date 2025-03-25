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

#include <nuttx/lib/math.h>
#include "leakdetector.h"

#define MAX_AGE_THRESHOLD (3000)
#define MAX_MM_UNFREED_COUNT (10)
#define MAX_MM_ACTIVE_SIZE (1024)

typedef enum
{
  TOO_MANY_UNFREED_ALLOC,
  OVER_MAX_ACTIVE_SIZE,
  LEAK_DEFAULT_ERR,
} LEAK_ERR;

struct rt_mem_info;
struct task_mem_stats;

int basic_meomory_leak_check(struct rt_mem_info *rt_info, struct task_mem_stats *tms);

void report_err(LEAK_ERR err, pid_t pid);

struct task_mem_stats;

float calc_leak_rate(const struct rt_mem_info *rt_info, const struct task_mem_stats *tms);

int is_basic_err(struct task_mem_stats *tms);

#endif /* __INCLUDE_NUTTX_MM_GRAN_H */