# ====================================================================
# OpenWrt 25.12 package: asterisk-chan-e5
#
# 编译方式对齐 OpenWrt 官方 chan-lantiq 模板：在 Build/Compile 里直接用
# $(TARGET_CC) 显式编译，不调用 src/Makefile（交叉环境下 $(CC) 不是
# $(TARGET_CC)，pkg-config 也常找不到 staging 的 glib/gio，故硬编码路径）。
#
# 放入 openwrt/package/ 或 feeds 下，然后：
#   make package/asterisk-chan-e5/{clean,compile} V=s -j1
# ====================================================================
include $(TOPDIR)/rules.mk

PKG_NAME:=asterisk-chan-e5
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=chan_e5 authors

# 纯 C 模块，不提供被其它包链接的共享库
PKG_FLAGS:=nonshared
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/asterisk-chan-e5
  SUBMENU:=Telephony
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Asterisk E5 (Unisoc SharkL5) cellular channel (ModemManager + AGDSP tap)
  # alsa-utils 提供 amixer/arecord/aplay（tap 脚本与探测需要）
  DEPENDS:=+asterisk +alsa-lib +glib2 +dbus +modemmanager +alsa-utils
endef

define Package/asterisk-chan-e5/description
  chan_e5.so — Asterisk 通道驱动：经 ModemManager D-Bus 控制 E5 蜂窝语音，
  经 AGDSP tap 端点 (hw:0,9 / hw:0,18) 桥接双向音频到 SIP。
  控制台：e5 call / e5 sms send / e5 show status / e5 show calls。
endef

# e5.conf 属用户配置，升级时保留
define Package/asterisk-chan-e5/conffiles
/etc/asterisk/e5.conf
endef

# ---------- 交叉编译的 include/lib 路径（硬编码 staging，不依赖 pkg-config） ----------
AST_INCLUDE_DIR:= $(STAGING_DIR)/usr/include
AST_LIB_DIR:=     $(STAGING_DIR)/usr/lib
GLIB_INCLUDE:=    $(STAGING_DIR)/usr/include/glib-2.0
GLIB_CONFIG_INC:= $(STAGING_DIR)/usr/lib/glib-2.0/include
GIO_INCLUDE:=     $(STAGING_DIR)/usr/include/gio-unix-2.0

COMMON_INCLUDES:= \
	-I$(AST_INCLUDE_DIR) \
	-I$(GLIB_INCLUDE) \
	-I$(GLIB_CONFIG_INC) \
	-I$(GIO_INCLUDE)

# asterisk.h 强制：每个 extern 模块源文件（含 e5_mm.c 等）都必须在包含
# asterisk.h 之前声明 AST_MODULE_SELF_SYM。故两者均全局 -D。
COMMON_CFLAGS:= \
	$(TARGET_CFLAGS) -pthread -fPIC \
	-DAST_MODULE_SELF_SYM=__chan_e5_module \
	-DAST_MODULE=\"chan_e5\" \
	$(COMMON_INCLUDES) \
	-Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wno-unused-parameter

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	cd $(PKG_BUILD_DIR) && \
	$(TARGET_CC) -c chan_e5.c  -MD -MT chan_e5.o  -MF .chan_e5.o.d  -MP $(COMMON_CFLAGS) && \
	$(TARGET_CC) -c e5_mm.c    -MD -MT e5_mm.o    -MF .e5_mm.o.d    -MP $(COMMON_CFLAGS) && \
	$(TARGET_CC) -c e5_audio.c -MD -MT e5_audio.o -MF .e5_audio.o.d -MP $(COMMON_CFLAGS) && \
	$(TARGET_CC) -c e5_route.c -MD -MT e5_route.o -MF .e5_route.o.d -MP $(COMMON_CFLAGS) && \
	$(TARGET_CC) -o chan_e5.so -pthread $(TARGET_LDFLAGS) -shared \
		-Wl,--version-script,chan_e5.exports,--warn-common \
		-L$(AST_LIB_DIR) \
		chan_e5.o e5_mm.o e5_audio.o e5_route.o \
		-lasound -lglib-2.0 -lgio-2.0 -lgobject-2.0 -lgthread-2.0 -lm
endef

define Package/asterisk-chan-e5/install
	$(INSTALL_DIR) $(1)/usr/lib/asterisk/modules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/chan_e5.so $(1)/usr/lib/asterisk/modules/
	$(INSTALL_DIR) $(1)/etc/asterisk
	$(INSTALL_CONF) ./configs/e5.conf $(1)/etc/asterisk/e5.conf
	$(INSTALL_DATA) ./configs/extensions_e5.conf $(1)/etc/asterisk/extensions_e5.conf
	$(INSTALL_DIR) $(1)/usr/libexec/asterisk
	$(INSTALL_BIN) ./files/usr/libexec/asterisk/e5-tap-up   $(1)/usr/libexec/asterisk/
	$(INSTALL_BIN) ./files/usr/libexec/asterisk/e5-tap-down $(1)/usr/libexec/asterisk/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/99-addgroups $(1)/etc/uci-defaults/99-addgroups
endef

$(eval $(call BuildPackage,asterisk-chan-e5))
