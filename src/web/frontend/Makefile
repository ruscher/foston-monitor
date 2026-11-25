#
# Frontend "bigfoston" do NetSurf (BigFoston Web): motor sem interface própria,
# controlado pelo shell do BigFoston OS por pipe e memória compartilhada.
# Reaproveita do frontend framebuffer os plotters, bitmaps, fontes FreeType,
# fetch e agendador (compilados de ../framebuffer), sem o toolkit fbtk.
# SPDX-License-Identifier: GPL-2.0-only
#

# -Dnsframebuffer: usa as opções do framebuffer (fb_face_*, fb_font_cachesize)
CFLAGS += -std=c99 -Dnsframebuffer -Dsmall -DFB_USE_FREETYPE

CFLAGS += '-DNETSURF_FB_RESPATH="$(NETSURF_FB_RESPATH)"'
CFLAGS += '-DNETSURF_FB_FONTPATH="$(NETSURF_FB_FONTPATH)"'
CFLAGS += '-DNETSURF_FB_FONT_SANS_SERIF="$(NETSURF_FB_FONT_SANS_SERIF)"'
CFLAGS += '-DNETSURF_FB_FONT_SANS_SERIF_BOLD="$(NETSURF_FB_FONT_SANS_SERIF_BOLD)"'
CFLAGS += '-DNETSURF_FB_FONT_SANS_SERIF_ITALIC="$(NETSURF_FB_FONT_SANS_SERIF_ITALIC)"'
CFLAGS += '-DNETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD="$(NETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD)"'
CFLAGS += '-DNETSURF_FB_FONT_SERIF="$(NETSURF_FB_FONT_SERIF)"'
CFLAGS += '-DNETSURF_FB_FONT_SERIF_BOLD="$(NETSURF_FB_FONT_SERIF_BOLD)"'
CFLAGS += '-DNETSURF_FB_FONT_MONOSPACE="$(NETSURF_FB_FONT_MONOSPACE)"'
CFLAGS += '-DNETSURF_FB_FONT_MONOSPACE_BOLD="$(NETSURF_FB_FONT_MONOSPACE_BOLD)"'
CFLAGS += '-DNETSURF_FB_FONT_CURSIVE="$(NETSURF_FB_FONT_CURSIVE)"'
CFLAGS += '-DNETSURF_FB_FONT_FANTASY="$(NETSURF_FB_FONT_FANTASY)"'

LDFLAGS += -lm
# build-web.sh arm: -Wl,-rpath,$$ORIGIN/lib (bibliotecas ao lado do binário, no cartão)
LDFLAGS += $(BFWEB_LDFLAGS)

$(eval $(call pkg_config_find_and_add,freetype2,freetype2))

# libnsfb inteira: as superfícies se registram por construtores
LDFLAGS += -Wl,--whole-archive
$(eval $(call pkg_config_find_and_add,libnsfb,libnsfb))
LDFLAGS += -Wl,--no-whole-archive

MESSAGES_FILTER=fb
MESSAGES_TARGET=$(FRONTEND_RESOURCES_DIR)

CFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE -D_DEFAULT_SOURCE

S_FRONTEND := gui.c download.c \
	../framebuffer/framebuffer.c ../framebuffer/schedule.c ../framebuffer/bitmap.c \
	../framebuffer/fetch.c ../framebuffer/findfile.c ../framebuffer/font_freetype.c

SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_FRONTEND)
EXETARGET := bfweb

install-bigfoston:

package-bigfoston:
