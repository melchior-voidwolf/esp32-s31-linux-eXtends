################################################################################
#
# dmenu
#
################################################################################

DMENU_VERSION = 5.3
DMENU_SITE = https://dl.suckless.org/tools
DMENU_LICENSE = MIT
DMENU_LICENSE_FILES = LICENSE
DMENU_DEPENDENCIES = fontconfig freetype xlib_libX11 xlib_libXft \
	xlib_libXinerama xlib_libXrender

# Command-line CFLAGS/LDFLAGS would override config.mk wholesale and lose
# INCS/LIBS, so only pass the variables config.mk expects us to override.
define DMENU_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		PREFIX=/usr \
		X11INC=$(STAGING_DIR)/usr/include \
		X11LIB=$(STAGING_DIR)/usr/lib \
		FREETYPEINC=$(STAGING_DIR)/usr/include/freetype2
endef

define DMENU_INSTALL_TARGET_CMDS
	$(MAKE) -C $(@D) PREFIX=$(TARGET_DIR)/usr install
endef

$(eval $(generic-package))
