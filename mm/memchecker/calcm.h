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

struct task_mem_stats;

int cal_unfreed_count(struct task_mem_stats *tms);

/** (活跃时间 * 活跃大小 + .... +  )  /  64 * 5s * 未释放次数   */
int cal_unfreed_chunck(struct task_mem_stats *tms);

/** 逻辑判断 内存活跃数量为0 但是却存在内存量 此时必然发生泄漏  */

void basic_test();

#endif /* __INCLUDE_NUTTX_MM_GRAN_H */