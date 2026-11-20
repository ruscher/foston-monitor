################################################################################
# bigfoston-shell: graphical shell (src/ui, LVGL 9.2.2)
################################################################################
BIGFOSTON_SHELL_VERSION = 0.1.0
BIGFOSTON_SHELL_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/ui
BIGFOSTON_SHELL_SITE_METHOD = local
BIGFOSTON_SHELL_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_SHELL_LICENSE = GPL-3.0-or-later, MIT (LVGL)
BIGFOSTON_SHELL_DEPENDENCIES = mpg123

# LVGL is not versioned in git: src/ui/tools/fetch_lvgl.sh downloads it
# (SHA-256 checked) and applies src/ui/patches/.
define BIGFOSTON_SHELL_BUILD_CMDS
	test -f $(@D)/third_party/lvgl/lvgl.h || $(@D)/tools/fetch_lvgl.sh
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) VARIANT=target \
		CROSS_COMPILE="$(TARGET_CROSS)" all size
endef

define BIGFOSTON_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/target/bigfoston-shell.stripped \
		$(TARGET_DIR)/usr/bin/bigfoston-shell
	mkdir -p $(TARGET_DIR)/usr/share/bigfoston
	cp -r $(@D)/data/apps $(TARGET_DIR)/usr/share/bigfoston/
endef

$(eval $(generic-package))
