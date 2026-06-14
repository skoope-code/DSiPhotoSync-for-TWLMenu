#---------------------------------------------------------------------------------
# DSiPhotoSync - devkitARM (libnds) Makefile
# Build with: make    (requires devkitPro + nds-dev installed, DEVKITPRO set)
# Produces DSiPhotoSync.nds
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. e.g. export DEVKITARM=$$DEVKITPRO/devkitARM")
endif

include $(DEVKITARM)/ds_rules

#---------------------------------------------------------------------------------
TARGET   := DSiPhotoSync
BUILD    := build
SOURCES  := source
INCLUDES := source

#---------------------------------------------------------------------------------
# Banner shown under the icon in TWiLight / the DSi menu, plus the 32x32 icon.
# Setting these overrides devkitPro's defaults (which is where the
# "www.devkitpro.org / www.drunkencoders.com" lines come from).
#---------------------------------------------------------------------------------
export GAME_TITLE     := DSiPhotoSync for
export GAME_SUBTITLE1 := TWiLight Menu++
export GAME_SUBTITLE2 := Skoope
# GAME_ICON is set in the top-level branch below, where $(CURDIR) is reliably
# the project root (in the recursive build/ make it would resolve wrongly).

# DSi mode is required for SD card access (DLDI/SD on the internal slot).
ARCH     := -mthumb -mthumb-interwork

CFLAGS   := -g -Wall -O2 -march=armv5te -mtune=arm946e-s \
            -ffunction-sections -fdata-sections \
            $(ARCH) $(INCLUDE) -DARM9
CFLAGS   += $(INCLUDE)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := -lfat -lnds9

LIBDIRS  := $(LIBNDS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export GAME_ICON := $(CURDIR)/icon.bmp
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nds $(TARGET).elf

#---------------------------------------------------------------------------------
else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nds : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
