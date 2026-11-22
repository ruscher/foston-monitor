################################################################################
# bigfoston-bt: Bluetooth bring-up tools (src/bt)
################################################################################
BIGFOSTON_BT_VERSION = 0.1.0
BIGFOSTON_BT_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/bt
BIGFOSTON_BT_SITE_METHOD = local
BIGFOSTON_BT_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_BT_LICENSE = GPL-3.0-or-later

define BIGFOSTON_BT_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS="$(TARGET_CROSS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define BIGFOSTON_BT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/bfbt-attach.stripped $(TARGET_DIR)/usr/sbin/bfbt-attach
	$(INSTALL) -D -m 0755 $(@D)/build/btprobe.stripped $(TARGET_DIR)/usr/sbin/btprobe
	$(INSTALL) -D -m 0755 $(@D)/build/bfhci.stripped $(TARGET_DIR)/usr/sbin/bfhci
endef

$(eval $(generic-package))
