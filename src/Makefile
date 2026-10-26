# ------------------------------------------------------------------
# chan_e5.so — 本地/SDK/OpenWrt 交叉编译
#
# 设备上直接编译（rootfs 已带 asterisk 头文件）：
#   make
#
# 交叉编译（E5 = aarch64 OpenWrt/musl）：
#   make CC=aarch64-openwrt-linux-musl-gcc \
#        AST_DEV_DIR=/path/to/staging/usr/include \
#        AST_LIB_DIR=/path/to/staging/usr/lib
#
# OpenWrt package 构建由顶层 Makefile 接管（见 ../Makefile 的 Build/Compile）。
# ------------------------------------------------------------------

AST_DEV_DIR ?= /usr/include
AST_LIB_DIR ?= /usr/lib
LIBAUDIO    ?= /usr

AST_MODULE_NAME := chan_e5

CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
CFLAGS  += -Wno-unused-parameter -fPIC -pthread
# asterisk.h 强制：每个 extern 模块源文件都需在包含 asterisk.h 前声明
# AST_MODULE_SELF_SYM，故全局 -D。
CFLAGS  += -DAST_MODULE_SELF_SYM=__chan_e5_module
CFLAGS  += -DAST_MODULE=\"$(AST_MODULE_NAME)\"
CFLAGS  += -I$(AST_DEV_DIR) -I$(LIBAUDIO)/include
CFLAGS  += $(shell pkg-config --cflags glib-2.0 gio-2.0 2>/dev/null || true)

LDFLAGS += -L$(AST_LIB_DIR) -L$(LIBAUDIO)/lib
LDFLAGS += -pthread
LDLIBS  += -lasound -lm $(shell pkg-config --libs glib-2.0 gio-2.0 2>/dev/null || true)

VERSION_SCRIPT := $(if $(wildcard $(AST_MODULE_NAME).exports),$(AST_MODULE_NAME).exports,../$(AST_MODULE_NAME).exports)

OBJS = chan_e5.o e5_mm.o e5_audio.o e5_route.o

all: chan_e5.so

chan_e5.so: $(OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS) $(LDLIBS) \
		$(if $(wildcard $(VERSION_SCRIPT)),-Wl,--version-script,$(VERSION_SCRIPT),) \
		-Wl,--warn-common

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: chan_e5.so
	install -Dm755 chan_e5.so $(DESTDIR)/usr/lib/asterisk/modules/chan_e5.so

clean:
	rm -f $(OBJS) chan_e5.so

.PHONY: all install clean
