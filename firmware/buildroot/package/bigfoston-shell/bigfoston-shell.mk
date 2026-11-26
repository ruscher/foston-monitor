################################################################################
# bigfoston-shell: graphical shell (src/ui, LVGL 9.2.2)
################################################################################
BIGFOSTON_SHELL_VERSION = 0.6.4
BIGFOSTON_SHELL_SITE = $(BR2_EXTERNAL_BIGFOSTON_PATH)/../../src/ui
BIGFOSTON_SHELL_SITE_METHOD = local
BIGFOSTON_SHELL_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build
BIGFOSTON_SHELL_LICENSE = GPL-3.0-or-later, MIT (LVGL), Zlib (nanosvg, lodepng)
BIGFOSTON_SHELL_DEPENDENCIES = mpg123

# LVGL and nanosvg are not versioned in git: src/ui/tools/fetch_lvgl.sh and
# fetch_nanosvg.sh download them (SHA-256 checked); LVGL gets src/ui/patches/.
define BIGFOSTON_SHELL_BUILD_CMDS
	test -f $(@D)/third_party/lvgl/lvgl.h || $(@D)/tools/fetch_lvgl.sh
	test -f $(@D)/third_party/nanosvg/nanosvg.h || $(@D)/tools/fetch_nanosvg.sh
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) VARIANT=target \
		CROSS_COMPILE="$(TARGET_CROSS)" all size
endef

define BIGFOSTON_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/target/bigfoston-shell.stripped \
		$(TARGET_DIR)/usr/bin/bigfoston-shell
	mkdir -p $(TARGET_DIR)/usr/share/bigfoston
	cp -r $(@D)/data/apps $(@D)/data/wallpapers $(TARGET_DIR)/usr/share/bigfoston/
endef

$(eval $(generic-package))
