################################################################################
# bigfoston-gps: loader for the device's own gps.dll (src/gps)
################################################################################
BIGFOSTON_GPS_VERSION = 0.4.0
BIGFOSTON_GPS_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/gps
BIGFOSTON_GPS_SITE_METHOD = local
BIGFOSTON_GPS_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_GPS_LICENSE = GPL-3.0-or-later

define BIGFOSTON_GPS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CROSS="$(TARGET_CROSS)"
endef

define BIGFOSTON_GPS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/bfgps.stripped $(TARGET_DIR)/usr/bin/bfgps
	$(INSTALL) -D -m 0644 $(@D)/gps-registry.txt $(TARGET_DIR)/etc/bigfoston/gps-registry.txt
endef

$(eval $(generic-package))
