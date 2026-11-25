# bigfoston-shell — interface gráfica do BigFoston OS (LVGL 9.2.2)
#
#   make sim                      simulador de PC (SDL2) + modo --screenshot
#   make target CROSS_COMPILE=arm-buildroot-linux-musleabi-
#                                 FOSTON FS-707DC: fbdev + evdev, ARMv6 soft-float
#   make target-hostcheck         compila o caminho do alvo (fbdev/evdev) com o gcc
#                                 do PC, sem flags ARM — valida o código do alvo
#   make headless                 simulador sem SDL (só --screenshot)
#   make screenshots              gera docs/images/ui/*.png
#   make run                      roda o simulador
#   make clean

VARIANT   ?= sim
JOBS      ?= $(shell nproc 2>/dev/null || echo 2)
BUILD     := build/$(VARIANT)
BIN       := $(BUILD)/bigfoston-shell
VERSION   := 0.4.0

LVGL_DIR  := third_party/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
APP_SRCS  := main.c $(wildcard core/*.c) $(wildcard screens/*.c) $(wildcard widgets/*.c)
# Big GPS engine (biggps/README.md): hot loops, built with -O2 even on the target
BIGGPS_SRCS := biggps/bgmap.c biggps/raster.c biggps/bgrender.c biggps/bgroute.c biggps/bgnav.c biggps/bgrouter.c biggps/bgicons.c biggps/bgsearch.c
ASSET_SRCS:= $(wildcard assets/fonts/*.c) $(wildcard assets/icons/*.c) $(wildcard assets/logo/*.c)

INCLUDES  := -I. -I$(LVGL_DIR) -Ithird_party/lodepng -Ithird_party/nanosvg -Iassets/icons -Iassets/logo
DEFS      := -DLV_CONF_INCLUDE_SIMPLE -DBIGFOSTON_VERSION=\"$(VERSION)\"
WARN      := -Wall -Wextra
# LVGL é código de terceiros: compila sem -Wextra para não poluir a saída
# -Wno-unused-but-set-variable: lv_buttonmatrix.c (LVGL 9.2.2) só usa txt_tot_i com LV_USE_ASSERT
LVGL_WARN := -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable

ifeq ($(VARIANT),sim)
  CC        ?= gcc
  SDL_CFLAGS:= $(shell pkg-config --cflags sdl2 2>/dev/null)
  SDL_LIBS  := $(shell pkg-config --libs sdl2 2>/dev/null)
  ifeq ($(SDL_LIBS),)
    $(error SDL2 não encontrado (pkg-config sdl2). Use "make headless" para o modo só-screenshot)
  endif
  DEFS      += -DBIGFOSTON_SIM -DBIGFOSTON_SIM_SDL -DBIGFOSTON_SCREENSHOT=1
  OPT       := -O2 -g
  EXTRA_CF  := $(SDL_CFLAGS)
  LDLIBS    := $(SDL_LIBS) -lm
  SCREENSHOT:= 1
else ifeq ($(VARIANT),headless)
  CC        ?= gcc
  DEFS      += -DBIGFOSTON_SIM -DBIGFOSTON_SCREENSHOT=1
  OPT       := -O2 -g
  LDLIBS    := -lm
  SCREENSHOT:= 1
else ifeq ($(VARIANT),target)
  CROSS_COMPILE ?=
  CC        := $(CROSS_COMPILE)gcc
  STRIP     := $(CROSS_COMPILE)strip
  # -mno-unaligned-access: defeito do MT3351 (STRH em endereço ímpar após COW perde bytes)
  ARCH_FLAGS?= -march=armv6 -mfloat-abi=soft -marm -mno-unaligned-access
  SCREENSHOT?= 0
  DEFS      += -DBIGFOSTON_TARGET -DBIGFOSTON_SCREENSHOT=$(SCREENSHOT) -DNDEBUG
  OPT       := -Os $(ARCH_FLAGS) -ffunction-sections -fdata-sections
  LDFLAGS   += $(ARCH_FLAGS) -Wl,--gc-sections
  LDLIBS    := -lm
else ifeq ($(VARIANT),target-host)
  CC        := gcc
  STRIP     := strip
  SCREENSHOT?= 0
  DEFS      += -DBIGFOSTON_TARGET -DBIGFOSTON_SCREENSHOT=$(SCREENSHOT) -DNDEBUG
  OPT       := -Os -ffunction-sections -fdata-sections
  LDFLAGS   += -Wl,--gc-sections
  LDLIBS    := -lm
else
  $(error VARIANT desconhecida: $(VARIANT))
endif

# lodepng: inflate dos EPUB (Livros) sempre; codificador PNG só para --screenshot
EXTRA_SRCS := third_party/lodepng/lodepng.c third_party/fsp/fsp.c
ifneq ($(SCREENSHOT),1)
  DEFS += -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK
endif

# MP3 (Multimídia): libmpg123. No alvo vem do Buildroot (estática, arm_nofpu);
# no PC, do sistema se existir. MPG123=0 compila sem (a tela diz que falta).
ifeq ($(filter target target-host,$(VARIANT)),)
  MPG123 ?= $(shell pkg-config --exists libmpg123 2>/dev/null && echo 1 || echo 0)
else
  MPG123 ?= 1
endif
ifeq ($(MPG123),1)
  DEFS   += -DBIGFOSTON_HAVE_MPG123
  LDLIBS += -lmpg123 -lpthread -lm
endif

CFLAGS_BASE := -std=gnu11 $(OPT) $(DEFS) $(INCLUDES) $(EXTRA_CF) $(CFLAGS)

APP_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS) $(BIGGPS_SRCS))
LVGL_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(LVGL_SRCS) $(ASSET_SRCS) $(EXTRA_SRCS))
DEPS       := $(APP_OBJS:.o=.d)

.PHONY: all sim headless target target-hostcheck screenshots run clean size test

all: $(BIN)

sim:
	@$(MAKE) --no-print-directory -j$(JOBS) VARIANT=sim all
headless:
	@$(MAKE) --no-print-directory -j$(JOBS) VARIANT=headless all
target:
	@$(MAKE) --no-print-directory -j$(JOBS) VARIANT=target all size
target-hostcheck:
	@$(MAKE) --no-print-directory -j$(JOBS) VARIANT=target-host all size

$(BIN): $(APP_OBJS) $(LVGL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "==> $@"

size: $(BIN)
	@$(STRIP) -o $(BIN).stripped $(BIN)
	@ls -l $(BIN).stripped | awk '{printf "==> binário stripped: %s bytes\n", $$5}'

$(BUILD)/%.o: %.c lv_conf.h
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS_BASE) $(WARN) -MMD -MP -c $< -o $@

$(BUILD)/biggps/%.o: biggps/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(subst -Os,-O2,$(CFLAGS_BASE)) $(WARN) -Wno-unused-parameter -MMD -MP -c $< -o $@

# LVGL, fontes, ícones e lodepng: sem -Wextra
$(BUILD)/$(LVGL_DIR)/%.o: $(LVGL_DIR)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(LVGL_WARN) -c $< -o $@
$(BUILD)/assets/%.o: assets/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(LVGL_WARN) -c $< -o $@
# núcleo do protocolo do Big Screen (cópia do bsmd): com os avisos do nosso código
$(BUILD)/third_party/fsp/%.o: third_party/fsp/%.c third_party/fsp/fsp.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(WARN) -c $< -o $@
$(BUILD)/third_party/lodepng/%.o: third_party/lodepng/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) -w -c $< -o $@

# Capturas de tela para a documentação, com pasta de dados de exemplo isolada
SHOT_DIR ?= ../../docs/images/ui
screenshots: sim
	@rm -rf build/shotdata && mkdir -p build/shotdata
	@cp -r data/apps build/shotdata/apps
	@python3 tools/make_sample_pictures.py build/shotdata/pictures > /dev/null
	@printf '$$ df -h /\n$$ free -m\nuname -a\n' > build/shotdata/favorites.txt
	@printf 'ls -la /data\n$$ dmesg | tail -n 20\ncat /proc/cpuinfo\n' > build/shotdata/history.txt
	@mkdir -p $(SHOT_DIR) && rm -f $(SHOT_DIR)/*.png
	BIGFOSTON_DATA=build/shotdata ./build/sim/bigfoston-shell --screenshot $(SHOT_DIR)

# testes de unidade no PC (ASan/UBSan)
HOST_CC ?= gcc
test:
	@rm -rf build/test/webdata && mkdir -p build/test/webdata
	$(HOST_CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -w -c third_party/lodepng/lodepng.c -o build/test/lodepng.o
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. \
		tests/test_webdata.c core/webdata.c -o build/test/test_webdata
	./build/test/test_webdata build/test/webdata
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. \
		tests/test_lz4dec.c core/lz4dec.c -llz4 -o build/test/test_lz4dec
	./build/test/test_lz4dec
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. \
		tests/test_kbdmap.c core/kbdmap.c -o build/test/test_kbdmap
	./build/test/test_kbdmap
	@rm -rf build/test/padmap && mkdir -p build/test/padmap
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. \
		tests/test_padmap.c core/padmap.c -o build/test/test_padmap
	./build/test/test_padmap build/test/padmap
	@rm -rf build/test/pkg && mkdir -p build/test/pkg
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. -Ithird_party/lodepng \
		tests/test_pkg.c core/pkg.c build/test/lodepng.o -o build/test/test_pkg
	./build/test/test_pkg $(CURDIR)/build/test/pkg
	@rm -rf build/test/appgroups && mkdir -p build/test/appgroups
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. \
		tests/test_appgroups.c core/appgroups.c -o build/test/test_appgroups
	./build/test/test_appgroups build/test/appgroups

run: sim
	./build/sim/bigfoston-shell

clean:
	rm -rf build

-include $(DEPS)
