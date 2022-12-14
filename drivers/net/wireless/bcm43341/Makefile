# bcmdhd
DHDCFLAGS = -Wall -Wstrict-prototypes -Dlinux -DBCMDRIVER                 \
	-DBCMDONGLEHOST -DUNRELEASEDCHIP -DBCMDMA32 -DBCMFILEIMAGE            \
	-DDHDTHREAD -DDHD_DEBUG -DSDTEST -DBDC -DTOE                          \
	-DDHD_BCMEVENTS -DSHOW_EVENTS -DPROP_TXSTATUS -DBCMDBG                \
	-DCUSTOMER_HW2 -DUSE_KTHREAD_API                                      \
	-DMMC_SDIO_ABORT -DBCMSDIO -DBCMLXSDMMC -DBCMPLATFORM_BUS -DWLP2P     \
	-DWIFI_ACT_FRAME -DARP_OFFLOAD_SUPPORT                                \
	-DKEEP_ALIVE -DGET_CUSTOM_MAC_ENABLE -DPKT_FILTER_SUPPORT             \
	-DEMBEDDED_PLATFORM -DPNO_SUPPORT                                     \
	-DDHD_USE_IDLECOUNT -DSET_RANDOM_MAC_SOFTAP -DROAM_ENABLE -DVSDB      \
	-DWL_CFG80211_VSDB_PRIORITIZE_SCAN_REQUEST                            \
	-DESCAN_RESULT_PATCH -DHT40_GO -DPASS_ARP_PACKET -DSDIO_CRC_ERROR_FIX \
	-DDHD_DONOT_FORWARD_BCMEVENT_AS_NETWORK_PKT                           \
	-Idrivers/net/wireless/bcmdhd -Idrivers/net/wireless/bcmdhd/include

ifeq ($(CONFIG_BCMDHD_WIFI_CONTROL_FUNC),y)
DHDCFLAGS += -DCONFIG_WIFI_CONTROL_FUNC
else
DHDCFLAGS += -DCUSTOM_OOB_GPIO_NUM=2
endif

# for supporting different type of bcm943341_wbfgn_x board.
DHDCFLAGS += -DNV_BCM943341_WBFGN_MULTI_MODULE_SUPPORT
DHDCFLAGS += -DQUEUE_BW
## Set dhd_dpd_thread priority to MAX to avoid starvation
DHDCFLAGS += -DCUSTOM_DPC_PRIO_SETTING=99

# claim only BCM4334 chip ID
DHDCFLAGS += -DCLAIM_FOR_BCM43341

ifeq ($(CONFIG_BCMDHD_HW_OOB),y)
DHDCFLAGS += -DHW_OOB -DOOB_INTR_ONLY
else
DHDCFLAGS += -DSDIO_ISR_THREAD
endif

ifeq ($(CONFIG_BCMDHD_INSMOD_NO_FW_LOAD),y)
DHDCFLAGS += -DENABLE_INSMOD_NO_FW_LOAD
endif

ifneq ($(CONFIG_DHD_USE_SCHED_SCAN),)
DHDCFLAGS += -DWL_SCHED_SCAN
endif

ifeq ($(CONFIG_BCMDHD_EDP_SUPPORT),y)
DHDCFLAGS += -DWIFIEDP
endif

DHDOFILES = src/aiutils.o src/bcmsdh_sdmmc_linux.o src/dhd_linux.o src/siutils.o src/bcmutils.o   \
	src/dhd_linux_sched.o src/dhd_sdio.o src/bcmwifi_channels.o src/bcmevent.o src/hndpmu.o   \
	src/bcmsdh.o src/dhd_cdc.o src/bcmsdh_linux.o src/dhd_common.o src/linux_osl.o            \
	src/bcmsdh_sdmmc.o src/dhd_custom_gpio.o src/sbutils.o src/wldev_common.o src/wl_android.o

obj-$(CONFIG_BCMDHD) += bcm43341.o
bcm43341-objs += $(DHDOFILES)

ifeq ($(CONFIG_BCMDHD_WEXT),y)
bcm43341-objs += src/wl_iw.o
DHDCFLAGS += -DSOFTAP -DWL_WIRELESS_EXT -DUSE_IW
endif

ifneq ($(CONFIG_CFG80211),)
bcm43341-objs += src/wl_cfg80211.o src/wl_cfgp2p.o src/wl_linux_mon.o src/dhd_cfg80211.o
DHDCFLAGS += -DWL_CFG80211 -DWL_CFG80211_STA_EVENT -DWL_ENABLE_P2P_IF
endif

EXTRA_CFLAGS = $(DHDCFLAGS)
ifeq ($(CONFIG_BCMDHD),m)
EXTRA_LDFLAGS += --strip-debug
endif
