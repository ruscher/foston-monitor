################################################################################
# bigfoston-bringup: bring-up test program (src/bringup/bftest.c)
################################################################################
BIGFOSTON_BRINGUP_VERSION = 0.1.0
BIGFOSTON_BRINGUP_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/bringup
BIGFOSTON_BRINGUP_SITE_METHOD = local
BIGFOSTON_BRINGUP_LICENSE = GPL-3.0-or-later

define BIGFOSTON_BRINGUP_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -static -O2 -Wall -Wextra -marm -o $(@D)/bftest $(@D)/bftest.c
endef

define BIGFOSTON_BRINGUP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bftest $(TARGET_DIR)/bin/bftest
endef

$(eval $(generic-package))
