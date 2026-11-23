################################################################################
# bigfoston-espeak-ng: eSpeak NG program for the Big GPS voice (data on the SD card)
################################################################################
BIGFOSTON_ESPEAK_NG_VERSION = 1.52.0
BIGFOSTON_ESPEAK_NG_SOURCE = espeak-ng-$(BIGFOSTON_ESPEAK_NG_VERSION).tar.gz
BIGFOSTON_ESPEAK_NG_SITE = https://github.com/espeak-ng/espeak-ng/archive/$(BIGFOSTON_ESPEAK_NG_VERSION)
BIGFOSTON_ESPEAK_NG_LICENSE = GPL-3.0+
BIGFOSTON_ESPEAK_NG_LICENSE_FILES = COPYING
BIGFOSTON_ESPEAK_NG_CONF_OPTS = \
	-DUSE_MBROLA=OFF -DUSE_LIBSONIC=OFF -DUSE_LIBPCAUDIO=OFF -DUSE_ASYNC=OFF \
	-DUSE_KLATT=OFF -DUSE_SPEECHPLAYER=OFF -DESPEAK_BUILD_MANPAGES=OFF -DBUILD_TESTING=OFF \
	-DBUILD_SHARED_LIBS=OFF

# only the program: the data compiler would run the target binary on the host
define BIGFOSTON_ESPEAK_NG_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(BR2_CMAKE) --build $(BIGFOSTON_ESPEAK_NG_BUILDDIR) \
		--target espeak-ng-bin -j$(PARALLEL_JOBS)
endef

define BIGFOSTON_ESPEAK_NG_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(BIGFOSTON_ESPEAK_NG_BUILDDIR)/src/espeak-ng \
		$(TARGET_DIR)/usr/bin/espeak-ng
endef

$(eval $(cmake-package))
