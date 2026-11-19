################################################################################
# bigfoston-bigscreen: Big Screen Monitor Display client (src/bigscreen)
################################################################################
BIGFOSTON_BIGSCREEN_VERSION = 0.1.0
BIGFOSTON_BIGSCREEN_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/bigscreen
BIGFOSTON_BIGSCREEN_SITE_METHOD = local
BIGFOSTON_BIGSCREEN_LICENSE = GPL-3.0-or-later

define BIGFOSTON_BIGSCREEN_BUILD_CMDS
	$(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra -std=c11" OUT=build
endef

define BIGFOSTON_BIGSCREEN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/bigfoston-bigscreen $(TARGET_DIR)/usr/bin/bigfoston-bigscreen
endef

$(eval $(generic-package))
