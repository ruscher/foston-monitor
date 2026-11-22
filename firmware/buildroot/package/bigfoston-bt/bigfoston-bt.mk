################################################################################
# bigfoston-bt: Bluetooth bring-up tools (src/bt)
################################################################################
BIGFOSTON_BT_VERSION = 0.1.0
BIGFOSTON_BT_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/bt
BIGFOSTON_BT_SITE_METHOD = local
BIGFOSTON_BT_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_BT_LICENSE = GPL-3.0-or-later

ifeq ($(BR2_PACKAGE_DBUS),y)
BIGFOSTON_BT_DEPENDENCIES += dbus host-pkgconf
BIGFOSTON_BT_AGENT = AGENT=1 \
	DBUS_CFLAGS="`$(PKG_CONFIG_HOST_BINARY) --cflags dbus-1`" \
	DBUS_LIBS="`$(PKG_CONFIG_HOST_BINARY) --libs dbus-1`"
define BIGFOSTON_BT_INSTALL_AGENT
	$(INSTALL) -D -m 0755 $(@D)/build/bfbt-agent.stripped $(TARGET_DIR)/usr/sbin/bfbt-agent
endef
endif

define BIGFOSTON_BT_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS="$(TARGET_CROSS)" LDFLAGS="$(TARGET_LDFLAGS)" \
		$(BIGFOSTON_BT_AGENT)
endef

define BIGFOSTON_BT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/bfbt-attach.stripped $(TARGET_DIR)/usr/sbin/bfbt-attach
	$(INSTALL) -D -m 0755 $(@D)/build/btprobe.stripped $(TARGET_DIR)/usr/sbin/btprobe
	$(INSTALL) -D -m 0755 $(@D)/build/bfhci.stripped $(TARGET_DIR)/usr/sbin/bfhci
	$(INSTALL) -D -m 0755 $(@D)/build/bfsnoop.stripped $(TARGET_DIR)/usr/sbin/bfsnoop
	$(BIGFOSTON_BT_INSTALL_AGENT)
endef

$(eval $(generic-package))
