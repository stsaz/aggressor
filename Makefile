# aggressor Makefile

AGG_DIR := .
FFBASE_DIR := ../ffbase
FFOS_DIR := ../ffos
PKG_VER :=
PKG_ARCH :=

include $(FFBASE_DIR)/test/makeconf

BIN := aggressor
PKG_DIR := aggressor-0
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	BIN := aggressor.exe
	PKG_DIR := aggressor
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif

CFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -pthread
CFLAGS += -DFFBASE_HAVE_FFERR_STR
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
ifeq "$(OS)" "windows"
	LINKFLAGS += -lws2_32
endif

BIN := aggressor
ifeq "$(OS)" "windows"
	BIN := aggressor.exe
endif

default: build
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) install

build: $(BIN)

DEPS := $(wildcard $(AGG_DIR)/src/*.h) \
	$(wildcard $(AGG_DIR)/src/util/*.h) \
	$(AGG_DIR)/Makefile

%.o: $(AGG_DIR)/src/%.c $(DEPS)
	$(C) $(CFLAGS) $< -o $@

$(BIN): main.o client.o
	$(LINK) $+ $(LINKFLAGS) -o $@

clean:
	rm -fv $(BIN) *.o

install:
	mkdir -p $(PKG_DIR)
	cp -ruv $(BIN) $(AGG_DIR)/README.md $(PKG_DIR)
ifeq "$(OS)" "windows"
	mv $(PKG_DIR)/README.md $(PKG_DIR)/README.txt
	unix2dos $(PKG_DIR)/README.txt
endif

package: aggressor-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT)

aggressor-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT): $(PKG_DIR)
	$(PKG_PACKER) $@ $<
