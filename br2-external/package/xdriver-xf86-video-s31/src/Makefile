# Build s31_drv.so against the Buildroot staging xorg headers.
# Runs inside the esp32s31-buildroot:bookworm container; see build-s31drv.sh.
SYSROOT ?= /br/korvo1/host/riscv32-buildroot-linux-musl/sysroot
CROSS   ?= /br/korvo1/host/bin/riscv32-buildroot-linux-musl-

CC      := $(CROSS)gcc
CFLAGS  := -O2 -fPIC -Wall -fno-strict-aliasing -fvisibility=hidden \
           -include xorg-server.h -DXSERVER_LIBPCIACCESS \
           -DPACKAGE_VERSION_MAJOR=0 -DPACKAGE_VERSION_MINOR=5 -DPACKAGE_VERSION_PATCHLEVEL=1 \
           -I$(SYSROOT)/usr/include/xorg -I$(SYSROOT)/usr/include \
           -I$(SYSROOT)/usr/include/pixman-1 -I$(SYSROOT)/usr/include/X11
LDFLAGS := -shared -fPIC -L$(SYSROOT)/usr/lib -lpciaccess

all: s31_drv.so

s31_drv.so: s31.c s31_exa.c fbaccel.c fbaccel.h dma2d_struct.h ppa_struct.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ s31.c s31_exa.c fbaccel.c

clean:
	rm -f s31_drv.so
