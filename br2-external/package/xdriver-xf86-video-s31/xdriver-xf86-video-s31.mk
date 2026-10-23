################################################################################
#
# xdriver-xf86-video-s31
#
################################################################################

XDRIVER_XF86_VIDEO_S31_VERSION = local
XDRIVER_XF86_VIDEO_S31_SITE = $(BR2_EXTERNAL_ESP32S31_PATH)/package/xdriver-xf86-video-s31/src
XDRIVER_XF86_VIDEO_S31_SITE_METHOD = local
XDRIVER_XF86_VIDEO_S31_DEPENDENCIES = xserver_xorg-server xdriver_xf86-video-fbdev libpciaccess

# The shipped Makefile builds s31_drv.so directly against the staging
# xorg SDK headers; no autotools needed for a single loadable module.
define XDRIVER_XF86_VIDEO_S31_BUILD_CMDS
	$(MAKE) -C $(@D) \
		SYSROOT=$(STAGING_DIR) \
		CROSS=$(TARGET_CROSS)
endef

define XDRIVER_XF86_VIDEO_S31_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/s31_drv.so \
		$(TARGET_DIR)/usr/lib/xorg/modules/drivers/s31_drv.so
endef

$(eval $(generic-package))
