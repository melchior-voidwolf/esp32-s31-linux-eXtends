################################################################################
#
# icewm
#
################################################################################

ICEWM_VERSION = 3.9.0
ICEWM_SITE = $(call github,ice-wm,icewm,$(ICEWM_VERSION))
ICEWM_LICENSE = GPL-2.0-or-later
ICEWM_LICENSE_FILES = COPYING

ICEWM_DEPENDENCIES = \
	fontconfig freetype imlib2 \
	xlib_libX11 xlib_libXext xlib_libXrender xlib_libXcomposite \
	xlib_libXcursor xlib_libXdamage xlib_libXfixes xlib_libXft \
	xlib_libXrandr xlib_libSM xlib_libICE

# 800x480 panel, English only, no sound, imlib2 for icons/backgrounds.
# SKIP_TRANSLATIONS=ON: the po/ machinery installs .mo files with absolute
# DESTINATIONs (po/playground), which host cmake >= 3.28 prefixes with
# DESTDIR into TARGET_DIR; the buildroot scanner rightly rejects that.  We
# are English-only (ENABLE_NLS=OFF), so turn the whole po processing off.
ICEWM_CONF_OPTS = \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCFGDIR=/etc/icewm \
	-DDOCDIR=/usr/share/doc/icewm \
	-DENABLE_NLS=OFF \
	-DSKIP_TRANSLATIONS=ON \
	-DCONFIG_I18N=OFF \
	-DCONFIG_IMLIB2=ON \
	-DCONFIG_XPM=OFF \
	-DCONFIG_LIBPNG=OFF \
	-DCONFIG_LIBJPEG=OFF \
	-DCONFIG_GDK_PIXBUF_XLIB=OFF \
	-DCONFIG_LIBRSVG=OFF \
	-DCONFIG_NANOSVG=OFF \
	-DCONFIG_EXTERNAL_TRAY=OFF \
	-DCONFIG_COREFONTS=OFF \
	-DENABLE_LTO=OFF

$(eval $(cmake-package))
