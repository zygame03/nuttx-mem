#include "mmdebug.h"
#include "utils.h"

const char *COLOR_TABLE[] = {
    "\033[0m",  // 重置
    "\033[31m", // 红色
    "\033[32m", // 绿色
    "\033[33m", // 黄色
    "\033[34m", // 蓝色
    "\033[35m", // 品红
    "\033[36m", // 青色
    "\033[37m"  // 白色
};

/****************************************************************************
 * @brief 将 UNIX 时间戳转换为格式化的日期时间字符串 (UTC)
 * @param timestamp 秒级 UNIX 时间戳
 * @param buffer 输出缓冲区（至少 20 字节）
 * @return 成功返回 0，失败返回 -1（无效时间戳或缓冲区不足）
 ****************************************************************************/
int timestamp_to_utc_str(uint64_t timestamp, char *buffer, size_t buf_size)
{
  /* 1. 验证输入有效性 */
  if (buf_size < 20)
  { // "YYYY-MM-DD HH:MM:SS\0" 需要至少 20 字节
    return -1;
  }
  /* 2. 转换为 struct tm（UTC 时间）*/
  time_t raw_time = (time_t)timestamp;
  struct tm timeinfo;

  /* 使用可重入版本避免静态缓冲区冲突（若 NuttX 支持 gmtime_r）*/
#if defined(CONFIG_LIBC_HAVE_GMTIME_R)
  if (gmtime_r(&raw_time, &timeinfo) == NULL)
  {
    return -1;
  }
#else
  /* 注意：标准 gmtime 非线程安全 */
  struct tm *tmp = gmtime(&raw_time);
  if (!tmp)
  {
    return -1;
  }
  memcpy(&timeinfo, tmp, sizeof(struct tm));
#endif

  /* 3. 格式化输出 */
  snprintf(buffer, buf_size,
           "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, // tm_year 从 1900 开始计数
           timeinfo.tm_mon + 1,     // tm_mon 范围 0-11
           timeinfo.tm_mday,
           timeinfo.tm_hour,
           timeinfo.tm_min,
           timeinfo.tm_sec);

  return 0;
}

/**
 * @brief 将秒级时间戳转换为格式化字符串
 * @param timestamp 输入的时间戳（秒，从1970-01-01起）
 * @param buffer 输出缓冲区（至少20字节）
 * @return 成功返回0，失败返回-1
 */
int format_timestamp(time_t timestamp, char *buffer, int choice)
{
  struct tm tm_struct;

  // 将时间戳转换为UTC时间（若需本地时间，使用 localtime_r()）
  switch (choice)
  {
  case 0:
    if (gmtime_r(&timestamp, &tm_struct) == NULL)
    {
      return -1; // 时间转换失败
    }
    break;
  case 1:
    if (localtime_r(&timestamp, &tm_struct) == NULL)
    {
      return -1; // 时间转换失败
    }
    break;
  }

  // 格式化输出：YYYY-MM-DD HH:MM:SS
  snprintf(buffer, 20,
           "%04d-%02d-%02d %02d:%02d:%02d",
           tm_struct.tm_year + 1900, // 年份从1900开始
           tm_struct.tm_mon + 1,     // 月份范围0-11 → 1-12
           tm_struct.tm_mday,
           tm_struct.tm_hour,
           tm_struct.tm_min,
           tm_struct.tm_sec);

  return 0;
}