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

include $(DEVKITPRO)/libnds/ds_rules

#-------------------------------------------------------------------------------
TARGET      := ScratchDS
BUILD       := build
SOURCES     := source source/core source/graphics source/audio source/input
INCLUDES    := source
DATA        :=
GRAPHICS    :=
AUDIO       :=

#-------------------------------------------------------------------------------
ARCH        := -mthumb -mthumb-interwork

CFLAGS      := -g -Wall -O2 -fomit-frame-pointer \
               -ffast-math \
               $(ARCH) \
               $(INCLUDE) \
               -DARM9

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=c++14

ASFLAGS     := -g $(ARCH)

LDFLAGS     := -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

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
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                    -L$(DEVKITPRO)/libnds/lib \
                    -L$(DEVKITPRO)/portlibs/nds/lib

.PHONY: $(BUILD) clean all

all: $(BUILD)

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
