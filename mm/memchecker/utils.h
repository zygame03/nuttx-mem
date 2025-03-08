#ifndef __INCLUDE_NUTTX_MM_UTILS_H
#define __INCLUDE_NUTTX_MM_UTILS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <time.h>
#include <stdio.h>

/****************************************************************************
 *  对应颜色输出
 *   更直观区别不同信息
 ****************************************************************************/
typedef enum
{
  COLOR_RESET,   // 重置颜色
  COLOR_RED,     // 红色
  COLOR_GREEN,   // 绿色
  COLOR_YELLOW,  // 黄色
  COLOR_BLUE,    // 蓝色
  COLOR_MAGENTA, // 品红
  COLOR_CYAN,    // 青色
  COLOR_WHITE    // 白色
} color;

/**
 * 声明COLOR_TABLE
 */
extern const char *COLOR_TABLE[];

/****************************************************************************
 * @brief 将 UNIX 时间戳转换为格式化的日期时间字符串 (UTC)
 ****************************************************************************/
int timestamp_to_utc_str(uint64_t timestamp, char *buffer, size_t buf_size);

/****************************************************************************
 * @brief  对应秒级时间戳，此时对精度的要求并不算高
 *  choice 目前支持两种选择
 *    choice == 0 ===>UTC时间
 *    choice == 1 ===>localtime
 ****************************************************************************/
int format_timestamp(time_t timestamp, char *buffer, int choice);
#endif