# aggressor Makefile

AGG_DIR := .
FFBASE_DIR := ../ffbase
FFOS_DIR := ../ffos

include $(FFBASE_DIR)/test/makeconf

CFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -pthread
CFLAGS += -I$(AGG_DIR)/src -I$(FFOS_DIR) -I$(FFBASE_DIR)
LINKFLAGS := -pthread
ifeq "$(OPT)" "0"
	CFLAGS += -DFF_DEBUG -O0 -g
else
	CFLAGS += -O3 -fno-strict-aliasing
	LINKFLAGS += -s
endif
ifneq "$(SSE42)" "0"
	CFLAGS += -msse4.2
endif

all: aggressor

DEPS := $(wildcard $(AGG_DIR)/src/*.h) \
	$(wildcard $(AGG_DIR)/src/util/*.h) \
	$(AGG_DIR)/Makefile

%.o: $(AGG_DIR)/src/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@

aggressor: main.o
	$(LINK) $(LINKFLAGS) $+ -o $@

clean:
	rm -fv aggressor *.o
