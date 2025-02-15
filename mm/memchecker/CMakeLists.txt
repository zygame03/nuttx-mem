set(SRCS memchecker.c)

ifeq ($(CONFIG_MM_MEMCHECKER),y)
  CSRCS += memchecker.c
  CFLAGS += -include($(TOPDIR)/include/nuttx/mm/memchecker.h)
endif

target_sources(mm PRIVATE ${SRCS})
