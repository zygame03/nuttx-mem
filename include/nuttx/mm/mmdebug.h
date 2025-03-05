#ifndef __INCLUDE_NUTTX_MMDEBUG_H
#define __INCLUDE_NUTTX_MMDEBUG_H

#include <stdio.h>
#include <nuttx/mm/utils.h>

#ifdef DEBUG_ENABLE
#define DEBUG(fmt, ...)                                                                            \
  do                                                                                               \
  {                                                                                                \
    fprintf(stderr, "%s[DEBUG] %s:%d: " fmt "%s\n",                                                \
            COLOR_TABLE[COLOR_BLUE], __FILE__, __LINE__, ##__VA_ARGS__, COLOR_TABLE[COLOR_RESET]); \
  } while (0)
#else
#define DEBUG(...)
#endif
#endif