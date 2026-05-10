#===============================================================================
# ScratchDS Makefile
# Requires: devkitARM r62+, libnds 2.x, libfat, maxmod
# Build: make
# Output: ScratchDS.nds (flash to R4 card)
#===============================================================================

.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

LD := arm-none-eabi-g++
include $(DEVKITARM)/ds_rules

#-------------------------------------------------------------------------------
TARGET      := ScratchDS
BUILD       := build
SOURCES     := source \
               source/core \
               source/graphics \
               source/audio \
               source/input \
               source/scratch_extension \
               source/ui
INCLUDES    := source
DATA        :=
GRAPHICS    :=
AUDIO       :=

LIBDIRS     := $(DEVKITPRO)/libnds

#-------------------------------------------------------------------------------
# Version info — embed in binary via preprocessor defines
#-------------------------------------------------------------------------------
VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_PATCH := 0
SCRATCHDS_VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Detect devkitARM version if possible
DEVKITARM_VER := $(shell arm-none-eabi-gcc --version 2>/dev/null | head -1 | cut -d' ' -f3)
ifeq ($(strip $(DEVKITARM_VER)),)
DEVKITARM_VER := unknown
endif

# Build timestamp
BUILD_DATE := $(shell date "+%Y-%m-%dT%H:%M")

#-------------------------------------------------------------------------------
ARCH        := -mthumb -mthumb-interwork

CFLAGS      := -g -Wall -O2 -fomit-frame-pointer \
               -ffast-math \
               $(ARCH) \
               $(INCLUDE) \
               -DARM9 \
               -DSCRATCHDS_VERSION=\"$(SCRATCHDS_VERSION)\" \
               -DSCRATCHDS_BUILD_DATE=\"$(BUILD_DATE)\" \
               -DDEVKITARM_VERSION=\"$(DEVKITARM_VER)\"

CXXFLAGS    := $(CFLAGS) -DJSMN_STATIC -fno-rtti -fno-exceptions -std=c++14 -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_CPP

ASFLAGS     := -g $(ARCH)

LDFLAGS     := -specs=ds_arm9.specs -g -Wl,-Map,$(notdir $*.map)

#-------------------------------------------------------------------------------
LIBS        := -lfat -lmm9 -lnds9

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(CURDIR)/$(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(CURDIR)/$(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(CURDIR)/$(dir)/*.s)))

export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES       :=  $(OFILES_SRC)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD) \
					$(DEVKITPRO)/libnds/include

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                    -L$(DEVKITPRO)/libnds/lib \
                    -L$(DEVKITPRO)/portlibs/nds/lib

.PHONY: $(BUILD) clean all version

all: $(BUILD)

version:
	@echo "ScratchDS v$(SCRATCHDS_VERSION) ($(BUILD_DATE))"
	@echo "devkitARM: $(DEVKITARM_VER)"

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo Cleaning...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nds

else

DEPENDS     :=  $(OFILES_SRC:.o=.d)

$(OUTPUT).nds   :   $(OUTPUT).elf
$(OUTPUT).elf   :   $(OFILES)

-include $(DEPENDS)

endif
