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
VERSION   := 0.1.0

LVGL_DIR  := third_party/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
APP_SRCS  := main.c $(wildcard core/*.c) $(wildcard screens/*.c) $(wildcard widgets/*.c)
ASSET_SRCS:= $(wildcard assets/fonts/*.c) $(wildcard assets/icons/*.c)

INCLUDES  := -I. -I$(LVGL_DIR) -Ithird_party/lodepng -Iassets/icons
DEFS      := -DLV_CONF_INCLUDE_SIMPLE -DBIGFOSTON_VERSION=\"$(VERSION)\"
WARN      := -Wall -Wextra
# LVGL é código de terceiros: compila sem -Wextra para não poluir a saída
LVGL_WARN := -Wall -Wno-unused-parameter -Wno-unused-function

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
  ARCH_FLAGS?= -march=armv6 -mfloat-abi=soft -marm
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

ifeq ($(SCREENSHOT),1)
  EXTRA_SRCS := third_party/lodepng/lodepng.c
endif

CFLAGS_BASE := -std=gnu11 $(OPT) $(DEFS) $(INCLUDES) $(EXTRA_CF) $(CFLAGS)

APP_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS))
LVGL_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(LVGL_SRCS) $(ASSET_SRCS) $(EXTRA_SRCS))
DEPS       := $(APP_OBJS:.o=.d)

.PHONY: all sim headless target target-hostcheck screenshots run clean size

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

# LVGL, fontes, ícones e lodepng: sem -Wextra
$(BUILD)/$(LVGL_DIR)/%.o: $(LVGL_DIR)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(LVGL_WARN) -c $< -o $@
$(BUILD)/assets/%.o: assets/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) $(LVGL_WARN) -c $< -o $@
$(BUILD)/third_party/lodepng/%.o: third_party/lodepng/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_BASE) -w -c $< -o $@

# Capturas de tela para a documentação, com pasta de dados de exemplo isolada
SHOT_DIR ?= ../../docs/images/ui
screenshots: sim
	@rm -rf build/shotdata && mkdir -p build/shotdata
	@cp -r data/apps build/shotdata/apps
	@printf '$$ df -h /\n$$ free -m\nuname -a\n' > build/shotdata/favorites.txt
	@printf 'ls -la /data\n$$ dmesg | tail -n 20\ncat /proc/cpuinfo\n' > build/shotdata/history.txt
	@mkdir -p $(SHOT_DIR) && rm -f $(SHOT_DIR)/*.png
	BIGFOSTON_DATA=build/shotdata ./build/sim/bigfoston-shell --screenshot $(SHOT_DIR)

run: sim
	./build/sim/bigfoston-shell

clean:
	rm -rf build

-include $(DEPS)
