export KBUILD_VERBOSE=1
ifeq ($(TARGET_BUILD_KERNEL_5_4),true)
$(info --------bluetooth driver------)
MTK_CHIP_PCM := TRUE
CONFIG_MP_WAKEUP_SOURCE_SYSFS_STAT := y
endif
#export KERNEL_SRC := /lib/modules/$(shell uname -r)/build

###############################################################################
# SDIO
###############################################################################
#ccflags-y += -DDBG=0
#ccflags-y += -I$(src)/os -I$(src)/os/linux/include
ifeq ($(MTK_CHIP_PCM), TRUE)
    ccflags-y += -DMTK_CHIP_PCM
endif

#VPATH = /opt/toolchains/gcc-linaro-aarch64-linux-gnu-4.9-2014.09_linux
SDIO_MOD_NAME = btmtksdio
SDIO_CFILES := \
	btmtk_sdio.c \
        btmtk_main.c
ifeq ($(CONFIG_AMAZON_A2DP_TS),y)
    SDIO_CFILES += btif_audio_ts.c
endif


$(SDIO_MOD_NAME)-objs := $(SDIO_CFILES:.c=.o)

ifeq ($(CONFIG_MP_WAKEUP_SOURCE_SYSFS_STAT), y)
        ccflags-y += -DCONFIG_MP_WAKEUP_SOURCE_SYSFS_STAT
else
endif

ifneq ($(filter abc123_ftv abc123 abc123, $(TARGET_PRODUCT)),)
    ccflags-y += -Dabc123_AML_TEMP_CHIP_RESET_FIX
endif

ccflags-y += -DANDROID_OS

###############################################################################
# Common
###############################################################################
obj-m := $(SDIO_MOD_NAME).o

PWD   := $(shell pwd)

ifeq ($(TARGET_BUILD_KERNEL_5_4),true)

CONFIGS_BUILD := -Wno-sometimes-uninitialized -Wno-uninitialized
CONFIGS += -Wno-error

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules "EXTRA_CFLAGS+=-I$(INCLUDE) -Wno-error $(CONFIGS_BUILD) $(EXTRA_INCLUDE) $(KBUILD_CFLAGS_MODULE)" $(CONFIGS)

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install
	mkdir -p ${OUT_DIR}/../vendor_lib/modules
	cd $(M)/; find -name "*.ko" -exec cp {} ${OUT_DIR}/../vendor_lib/modules/ \;

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

else

all:
	make -C $(LINUX_SRC) M=$(PWD) modules

sdio:
	make -C $(LINUX_SRC) M=$(PWD) $(SDIO_MOD_NAME).ko
clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions bin2h
	rm -f Module.markers
	rm -f Module.symvers
	rm -f modules.order

# Check coding style
export IGNORE_CODING_STYLE_RULES := NEW_TYPEDEFS,LEADING_SPACE,CODE_INDENT,SUSPECT_CODE_INDENT
ccs:
	./scripts/checkpatch.pl -f btmtk_main.c
	./scripts/checkpatch.pl -f btmtk_sdio.c
	./scripts/checkpatch.pl -f btmtk_sdio.h
	./scripts/checkpatch.pl -f btmtk_config.h
	./scripts/checkpatch.pl -f btmtk_define.h
	./scripts/checkpatch.pl -f btmtk_drv.h
endif
