#ifndef __INCLUDE_NUTTX_MMDEBUG_H
#define __INCLUDE_NUTTX_MMDEBUG_H

#include <stdio.h>
#include "utils.h"

#ifdef INFO_ENABLE
#define INFO(fmt, ...)                                                                                        \
  do                                                                                                          \
  {                                                                                                           \
    fprintf(stderr, "%s[INFO] [%s-%s()]:%d: " fmt "%s\n",                                                     \
            COLOR_TABLE[COLOR_GREEN], __FILE__, __func__, __LINE__, ##__VA_ARGS__, COLOR_TABLE[COLOR_RESET]); \
  } while (0)
#else
#define INFO(...)
#endif

#ifdef DEBUG_ENABLE
#define DEBUG(fmt, ...)                                                                                      \
  do                                                                                                         \
  {                                                                                                          \
    fprintf(stderr, "%s[DEBUG] [%s-%s()]:%d: " fmt "%s\n",                                                   \
            COLOR_TABLE[COLOR_BLUE], __FILE__, __func__, __LINE__, ##__VA_ARGS__, COLOR_TABLE[COLOR_RESET]); \
  } while (0)
#else
#define DEBUG(...)
#endif
#ifdef WARN_ENABLE
#define WARN(fmt, ...)                                                                            \
  do                                                                                              \
  {                                                                                               \
    fprintf(stderr, "%s[WARN] %s:%d: " fmt "%s\n",                                                \
            COLOR_TABLE[COLOR_RED], __FILE__, __LINE__, ##__VA_ARGS__, COLOR_TABLE[COLOR_RESET]); \
  } while (0)
#else
#define WARN(...)
#endif
#endif