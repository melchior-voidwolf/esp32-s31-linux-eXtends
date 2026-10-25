#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs on the copy of the target directory that each image is made from.
# A change here does not touch target/, so the next build starts from the
# complete tree.
set -eu

# ca-certificates is optional (the small headless profile omits it).  When
# present, keep the bundle and remove duplicate per-certificate links.
rm -rf "$1/usr/share/ca-certificates"
if [ -d "$1/etc/ssl/certs" ]; then
	find "$1/etc/ssl/certs" -type l -delete
fi
if [ -f "$1/etc/ssl/certs/ca-certificates.crt" ]; then
	ln -sf certs/ca-certificates.crt "$1/etc/ssl/cert.pem"
fi

# Packages install libraries that nothing here uses: glib2 five next to
# libglib (and its tools), libnl, ncurses and readline their extra parts,
# BlueZ a libbluetooth that its programs do not link.  Remove the glib2
# tools, then each library that no ELF file in the image needs.  A library
# comes before the ones it needs.  OpenSSL modules and libgcc_s are not on
# the list: programs load them with dlopen.
readelf --version >/dev/null
cd "$1"
rm -f usr/bin/gapplication usr/bin/gdbus usr/bin/gi-compile-repository \
	usr/bin/gi-decompile-typelib usr/bin/gi-inspect-typelib usr/bin/gio \
	usr/bin/gio-querymodules usr/bin/gresource usr/bin/gsettings \
	usr/bin/pcre2test usr/bin/pcre2grep usr/bin/dbus-test-tool usr/bin/aserver \
	usr/libexec/gio-launch-desktop
rm -rf usr/lib/gio usr/share/glib-2.0
for lib in libgirepository-2.0 libgio-2.0 libgobject-2.0 libgmodule-2.0 \
	libgthread-2.0 libffi libnl-nf-3 libnl-xfrm-3 libnl-idiag-3 libform \
	libmenu libpanel libhistory libbluetooth; do
	for f in usr/lib/"$lib".so.*; do
		[ -f "$f" ] && [ ! -L "$f" ] || continue
		so=$(readelf -d "$f" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
		find . -type f -exec readelf -d {} + 2>/dev/null |
			grep -q "Shared library: \[$so\]" || rm -f "usr/lib/$lib".so*
	done
done
