################################################################################
# bigfoston-fm: transmissor FM do FOSTON (KT0801A; src/fm, docs/fm-radio)
################################################################################
BIGFOSTON_FM_VERSION = 0.1.0
BIGFOSTON_FM_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/fm
BIGFOSTON_FM_SITE_METHOD = local
BIGFOSTON_FM_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_FM_LICENSE = GPL-3.0-or-later

define BIGFOSTON_FM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS="$(TARGET_CROSS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define BIGFOSTON_FM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/bffm.stripped $(TARGET_DIR)/usr/sbin/bffm
endef

$(eval $(generic-package))
