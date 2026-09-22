#---------------------------------------------------------------------------------
# SONIC DASH -- Nintendo Switch homebrew loader (wrapper port)
# Unity 6000.0.72f1 / IL2CPP / arm64-v8a.  Retargeted from the colorsheep_nx /
# vln_nx so-loader lineage (MIT).  Ships NO game code or assets.
#
# Requires devkitA64 + devkitPro packages:
#   pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := sonicdash_nx
APP_TITLE   := Sonic Dash
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.1
APP_ICON    := $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           -DCRASH_LOG_PRINTF=debugPrintf \
           $(INCLUDE) -D__SWITCH__
# The reserved region where the game .so are loaded (keep in sync with so_util.c).
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# mesa GLES3 + EGL + nouveau (force the GLES path); SDL2 for window/HID/audio; zlib.
LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES  := $(addsuffix .o,$(SFILES)) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --nacp=$(OUTPUT).nacp
# The icon is optional: elf2nro falls back to the default homebrew icon. It was
# previously passed unconditionally for a file this repo did not contain, which
# failed the very last build step.
ifneq ($(wildcard $(APP_ICON)),)
NROFLAGS += --icon=$(APP_ICON)
endif
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
