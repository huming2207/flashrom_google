#
# This file is part of the flashrom project.
#
# Copyright (C) 2005 coresystems GmbH <stepan@coresystems.de>
# Copyright (C) 2009,2010,2012 Carl-Daniel Hailfinger
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
#

PROGRAM = flashrom

###############################################################################
# Defaults for the toolchain.

# If you want to cross-compile, just run e.g.
# make CC=i586-pc-msdosdjgpp-gcc
# You may have to specify STRIP/AR/RANLIB as well.
#
# Note for anyone editing this Makefile: gnumake will happily ignore any
# changes in this Makefile to variables set on the command line.
STRIP   ?= strip
INSTALL = install
DIFF    = diff
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man
CFLAGS  ?= -Os -Wall -Wshadow
EXPORTDIR ?= .
RANLIB  ?= ranlib
PKG_CONFIG ?= pkg-config
BUILD_DETAILS_FILE ?= build_details.txt

# If your compiler spits out excessive warnings, run make WARNERROR=no
# You shouldn't have to change this flag.
WARNERROR ?= yes

ifeq ($(WARNERROR), yes)
CFLAGS += -Werror
endif

ifdef LIBS_BASE
PKG_CONFIG_LIBDIR ?= $(LIBS_BASE)/lib/pkgconfig
override CPPFLAGS += -I$(LIBS_BASE)/include
override LDFLAGS += -L$(LIBS_BASE)/lib -Wl,-rpath -Wl,$(LIBS_BASE)/lib
endif

ifeq ($(CONFIG_STATIC),yes)
override PKG_CONFIG += --static
override LDFLAGS += -static
endif

# Set LC_ALL=C to minimize influences of the locale.
# However, this won't work for the majority of relevant commands because they use the $(shell) function and
# GNU make does not relay variables exported within the makefile to their evironment.
LC_ALL=C
export LC_ALL

dummy_for_make_3_80:=$(shell printf "Build started on %s\n\n" "$$(date)" >$(BUILD_DETAILS_FILE))

# Provide an easy way to execute a command, print its output to stdout and capture any error message on stderr
# in the build details file together with the original stdout output.
debug_shell = $(shell export LC_ALL=C ; { echo 'exec: export LC_ALL=C ; { $(1) ; }' >&2;  { $(1) ; } | tee -a $(BUILD_DETAILS_FILE) ; echo >&2 ; } 2>>$(BUILD_DETAILS_FILE))

###############################################################################
# General OS-specific settings.
# 1. Prepare for later by gathering information about host and target OS
# 2. Set compiler flags and parameters according to OSes
# 3. Likewise verify user-supplied CONFIG_* variables.

# HOST_OS is only used to work around local toolchain issues.
HOST_OS ?= $(shell uname)
ifeq ($(HOST_OS), MINGW32_NT-5.1)
# Explicitly set CC = gcc on MinGW, otherwise: "cc: command not found".
CC = gcc
endif
ifneq ($(HOST_OS), SunOS)
STRIP_ARGS = -s
endif

# Determine the destination OS.
# IMPORTANT: The following line must be placed before TARGET_OS is ever used
# (of course), but should come after any lines setting CC because the line
# below uses CC itself.
override TARGET_OS := $(strip $(call debug_shell,$(CC) $(CPPFLAGS) -E os.h 2>/dev/null | grep -v '^\#' | grep '"' | cut -f 2 -d'"'))

ifeq ($(TARGET_OS), Darwin)
override CPPFLAGS += -I/opt/local/include -I/usr/local/include
override LDFLAGS += -L/opt/local/lib -L/usr/local/lib
endif

ifeq ($(TARGET_OS), FreeBSD)
override CPPFLAGS += -I/usr/local/include
override LDFLAGS += -L/usr/local/lib
endif

ifeq ($(TARGET_OS), OpenBSD)
override CPPFLAGS += -I/usr/local/include
override LDFLAGS += -L/usr/local/lib
endif

ifeq ($(TARGET_OS), NetBSD)
override CPPFLAGS += -I/usr/pkg/include
override LDFLAGS += -L/usr/pkg/lib
endif

ifeq ($(TARGET_OS), DragonFlyBSD)
override CPPFLAGS += -I/usr/local/include
override LDFLAGS += -L/usr/local/lib
endif

ifeq ($(TARGET_OS), DOS)
EXEC_SUFFIX := .exe
# DJGPP has odd uint*_t definitions which cause lots of format string warnings.
override CFLAGS += -Wno-format
LIBS += -lgetopt
# Bus Pirate, Serprog and PonyProg are not supported under DOS (missing serial support).
ifeq ($(CONFIG_BUSPIRATE_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_BUSPIRATE_SPI=yes
else
override CONFIG_BUSPIRATE_SPI = no
endif
ifeq ($(CONFIG_SERPROG), yes)
UNSUPPORTED_FEATURES += CONFIG_SERPROG=yes
else
override CONFIG_SERPROG = no
endif
ifeq ($(CONFIG_PONY_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_PONY_SPI=yes
else
override CONFIG_PONY_SPI = no
endif
# Dediprog, USB-Blaster, PICkit2, CH341A and FT2232 are not supported under DOS (missing USB support).
ifeq ($(CONFIG_DEDIPROG), yes)
UNSUPPORTED_FEATURES += CONFIG_DEDIPROG=yes
else
override CONFIG_DEDIPROG = no
endif
ifeq ($(CONFIG_FT2232_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_FT2232_SPI=yes
else
override CONFIG_FT2232_SPI = no
endif
ifeq ($(CONFIG_USBBLASTER_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_USBBLASTER_SPI=yes
else
override CONFIG_USBBLASTER_SPI = no
endif
ifeq ($(CONFIG_PICKIT2_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_PICKIT2_SPI=yes
else
override CONFIG_PICKIT2_SPI = no
endif
ifeq ($(CONFIG_CH341A_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_CH341A_SPI=yes
else
override CONFIG_CH341A_SPI = no
endif
endif

# FIXME: Should we check for Cygwin/MSVC as well?
ifeq ($(TARGET_OS), MinGW)
EXEC_SUFFIX := .exe
# MinGW doesn't have the ffs() function, but we can use gcc's __builtin_ffs().
CFLAGS += -Dffs=__builtin_ffs
# libusb-win32/libftdi stuff is usually installed in /usr/local.
CPPFLAGS += -I/usr/local/include
LDFLAGS += -L/usr/local/lib
# Serprog is not supported under Windows/MinGW (missing sockets support).
ifeq ($(CONFIG_SERPROG), yes)
UNSUPPORTED_FEATURES += CONFIG_SERPROG=yes
else
override CONFIG_SERPROG = no
endif
# For now we disable all PCI-based programmers on Windows/MinGW (no libpci).
ifeq ($(CONFIG_INTERNAL), yes)
UNSUPPORTED_FEATURES += CONFIG_INTERNAL=yes
else
override CONFIG_INTERNAL = no
endif
ifeq ($(CONFIG_RAYER_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_RAYER_SPI=yes
else
override CONFIG_RAYER_SPI = no
endif
ifeq ($(CONFIG_NIC3COM), yes)
UNSUPPORTED_FEATURES += CONFIG_NIC3COM=yes
else
override CONFIG_NIC3COM = no
endif
ifeq ($(CONFIG_GFXNVIDIA), yes)
UNSUPPORTED_FEATURES += CONFIG_GFXNVIDIA=yes
else
override CONFIG_GFXNVIDIA = no
endif
ifeq ($(CONFIG_SATASII), yes)
UNSUPPORTED_FEATURES += CONFIG_SATASII=yes
else
override CONFIG_SATASII = no
endif
ifeq ($(CONFIG_ATAHPT), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAHPT=yes
else
override CONFIG_ATAHPT = no
endif
ifeq ($(CONFIG_ATAVIA), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAVIA=yes
else
override CONFIG_ATAVIA = no
endif
ifeq ($(CONFIG_ATAPROMISE), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAPROMISE=yes
else
override CONFIG_ATAPROMISE = no
endif
ifeq ($(CONFIG_IT8212), yes)
UNSUPPORTED_FEATURES += CONFIG_IT8212=yes
else
override CONFIG_IT8212 = no
endif
ifeq ($(CONFIG_DRKAISER), yes)
UNSUPPORTED_FEATURES += CONFIG_DRKAISER=yes
else
override CONFIG_DRKAISER = no
endif
ifeq ($(CONFIG_NICREALTEK), yes)
UNSUPPORTED_FEATURES += CONFIG_NICREALTEK=yes
else
override CONFIG_NICREALTEK = no
endif
ifeq ($(CONFIG_NICNATSEMI), yes)
UNSUPPORTED_FEATURES += CONFIG_NICNATSEMI=yes
else
override CONFIG_NICNATSEMI = no
endif
ifeq ($(CONFIG_NICINTEL), yes)
UNSUPPORTED_FEATURES += CONFIG_NICINTEL=yes
else
override CONFIG_NICINTEL = no
endif
ifeq ($(CONFIG_NICINTEL_EEPROM), yes)
UNSUPPORTED_FEATURES += CONFIG_NICINTEL_EEPROM=yes
else
override CONFIG_NICINTEL_EEPROM = no
endif
ifeq ($(CONFIG_NICINTEL_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_NICINTEL_SPI=yes
else
override CONFIG_NICINTEL_SPI = no
endif
ifeq ($(CONFIG_OGP_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_OGP_SPI=yes
else
override CONFIG_OGP_SPI = no
endif
ifeq ($(CONFIG_SATAMV), yes)
UNSUPPORTED_FEATURES += CONFIG_SATAMV=yes
else
override CONFIG_SATAMV = no
endif
endif

ifeq ($(TARGET_OS), libpayload)
ifeq ($(MAKECMDGOALS),)
.DEFAULT_GOAL := libflashrom.a
$(info Setting default goal to libflashrom.a)
endif
CPPFLAGS += -DSTANDALONE
ifeq ($(CONFIG_DUMMY), yes)
UNSUPPORTED_FEATURES += CONFIG_DUMMY=yes
else
override CONFIG_DUMMY = no
endif
# libpayload does not provide the romsize field in struct pci_dev that the atapromise code requires.
ifeq ($(CONFIG_ATAPROMISE), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAPROMISE=yes
else
override CONFIG_ATAPROMISE = no
endif
# Bus Pirate, Serprog and PonyProg are not supported with libpayload (missing serial support).
ifeq ($(CONFIG_BUSPIRATE_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_BUSPIRATE_SPI=yes
else
override CONFIG_BUSPIRATE_SPI = no
endif
ifeq ($(CONFIG_SERPROG), yes)
UNSUPPORTED_FEATURES += CONFIG_SERPROG=yes
else
override CONFIG_SERPROG = no
endif
ifeq ($(CONFIG_PONY_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_PONY_SPI=yes
else
override CONFIG_PONY_SPI = no
endif
# Dediprog, USB-Blaster, PICkit2, CH341A and FT2232 are not supported with libpayload (missing libusb support).
ifeq ($(CONFIG_DEDIPROG), yes)
UNSUPPORTED_FEATURES += CONFIG_DEDIPROG=yes
else
override CONFIG_DEDIPROG = no
endif
ifeq ($(CONFIG_FT2232_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_FT2232_SPI=yes
else
override CONFIG_FT2232_SPI = no
endif
ifeq ($(CONFIG_USBBLASTER_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_USBBLASTER_SPI=yes
else
override CONFIG_USBBLASTER_SPI = no
endif
ifeq ($(CONFIG_PICKIT2_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_PICKIT2_SPI=yes
else
override CONFIG_PICKIT2_SPI = no
endif
ifeq ($(CONFIG_CH341A_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_CH341A_SPI=yes
else
override CONFIG_CH341A_SPI = no
endif
endif

ifneq ($(TARGET_OS), Linux)
# Android is handled internally as separate OS, but it supports CONFIG_LINUX_SPI and CONFIG_MSTARDDC_SPI
ifneq ($(TARGET_OS), Android)
ifeq ($(CONFIG_LINUX_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_LINUX_SPI=yes
else
override CONFIG_LINUX_SPI = no
endif
ifeq ($(CONFIG_MSTARDDC_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_MSTARDDC_SPI=yes
else
override CONFIG_MSTARDDC_SPI = no
endif
endif
endif

ifeq ($(TARGET_OS), Android)
# Android on x86 (currently) does not provide raw PCI port I/O operations
ifeq ($(CONFIG_RAYER_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_RAYER_SPI=yes
else
override CONFIG_RAYER_SPI = no
endif
endif

###############################################################################
# General architecture-specific settings.
# Like above for the OS, below we verify user-supplied options depending on the target architecture.

# Determine the destination processor architecture.
# IMPORTANT: The following line must be placed before ARCH is ever used
# (of course), but should come after any lines setting CC because the line
# below uses CC itself.
override ARCH := $(strip $(call debug_shell,$(CC) $(CPPFLAGS) -E archtest.c 2>/dev/null | grep -v '^\#' | grep '"' | cut -f 2 -d'"'))

# PCI port I/O support is unimplemented on PPC/MIPS/SPARC and unavailable on ARM.
# Right now this means the drivers below only work on x86.
ifneq ($(ARCH), x86)
ifeq ($(CONFIG_NIC3COM), yes)
UNSUPPORTED_FEATURES += CONFIG_NIC3COM=yes
else
override CONFIG_NIC3COM = no
endif
ifeq ($(CONFIG_NICREALTEK), yes)
UNSUPPORTED_FEATURES += CONFIG_NICREALTEK=yes
else
override CONFIG_NICREALTEK = no
endif
ifeq ($(CONFIG_NICNATSEMI), yes)
UNSUPPORTED_FEATURES += CONFIG_NICNATSEMI=yes
else
override CONFIG_NICNATSEMI = no
endif
ifeq ($(CONFIG_RAYER_SPI), yes)
UNSUPPORTED_FEATURES += CONFIG_RAYER_SPI=yes
else
override CONFIG_RAYER_SPI = no
endif
ifeq ($(CONFIG_ATAHPT), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAHPT=yes
else
override CONFIG_ATAHPT = no
endif
ifeq ($(CONFIG_ATAPROMISE), yes)
UNSUPPORTED_FEATURES += CONFIG_ATAPROMISE=yes
else
override CONFIG_ATAPROMISE = no
endif
ifeq ($(CONFIG_SATAMV), yes)
UNSUPPORTED_FEATURES += CONFIG_SATAMV=yes
else
override CONFIG_SATAMV = no
endif
endif

CHIP_OBJS = jedec.o stm50flw0x0x.o w39.o w29ee011.o \
	sst28sf040.o m29f400bt.o 82802ab.o pm49fl00x.o \
	sst49lfxxxc.o sst_fwhub.o flashchips.o spi.o spi25.o sharplhf00l04.o \
	a25.o at25.o s25f.o opaque.o writeprotect.o

LIB_OBJS = android.o layout.o file.o fmap.o power.o search.o

ifeq ($(CONFIG_FDTMAP), yes)
FEATURE_CFLAGS += -D'CONFIG_FDTMAP=1'
LIB_OBJS += fdtmap.o
ifeq ($(CONFIG_STATIC), yes)
LIBS += -static -lfdt -lz
else
LIBS += -lfdt -lz
endif
endif

LOCK_OBJS = big_lock.o file_lock.o cros_ec_lock.o
LIB_OBJS += $(LOCK_OBJS)
FEATURE_CFLAGS += -D'USE_BIG_LOCK=1' -D'USE_CROS_EC_LOCK=1'

CLI_OBJS = flashrom.o cli_mfg.o cli_output.o print.o

PROGRAMMER_OBJS = udelay.o programmer.o

all: pciutils features $(PROGRAM)$(EXEC_SUFFIX) $(PROGRAM).8
ifeq ($(ARCH), x86)
	@+$(MAKE) -C util/ich_descriptors_tool/ TARGET_OS=$(TARGET_OS) EXEC_SUFFIX=$(EXEC_SUFFIX)
endif

# Set the flashrom version string from the highest revision number
# of the checked out flashrom files.
# Note to packagers: Any tree exported with "make export" or "make tarball"
# will not require subversion. The downloadable snapshots are already exported.
SVNVERSION := $(shell ./util/getrevision.sh)

RELEASE := 0.9.4
VERSION := $(RELEASE) $(SVNVERSION)
RELEASENAME ?= $(VERSION)

SVNDEF := -D'FLASHROM_VERSION="$(VERSION)"'

# Always enable internal/onboard support for now.
CONFIG_INTERNAL ?= yes

# Always enable serprog for now. Needs to be disabled on Windows.
CONFIG_SERPROG ?= no

# RayeR SPIPGM hardware support
CONFIG_RAYER_SPI ?= no

# Always enable 3Com NICs for now.
CONFIG_NIC3COM ?= no

# Enable NVIDIA graphics cards. Note: write and erase do not work properly.
CONFIG_GFXNVIDIA ?= no

# Always enable SiI SATA controllers for now.
CONFIG_SATASII ?= no

# Highpoint (HPT) ATA/RAID controller support.
# IMPORTANT: This code is not yet working!
CONFIG_ATAHPT ?= no

# Always enable FT2232 SPI dongles for now.
CONFIG_FT2232_SPI ?= yes

# Always enable dummy tracing for now.
CONFIG_DUMMY ?= yes

# Always enable Dr. Kaiser for now.
CONFIG_DRKAISER ?= no

# Always enable Realtek NICs for now.
CONFIG_NICREALTEK ?= no

# Disable National Semiconductor NICs until support is complete and tested.
CONFIG_NICNATSEMI ?= no

# Always enable Intel NICs for now.
CONFIG_NICINTEL ?= no

# Always enable SPI on Intel NICs for now.
CONFIG_NICINTEL_SPI ?= no

# Always enable SPI on OGP cards for now.
CONFIG_OGP_SPI ?= no

# Always enable Bus Pirate SPI for now.
CONFIG_BUSPIRATE_SPI ?= no

# Raiden Debug SPI-over-USB support.
CONFIG_RAIDEN_DEBUG_SPI ?= no

# Enable Linux I2C for ChromeOS EC
CONFIG_LINUX_I2C ?= no

CONFIG_LINUX_MTD ?= no

# Always enable Dediprog SF100 for now.
CONFIG_DEDIPROG ?= yes

# Always enable Marvell SATA controllers for now.
CONFIG_SATAMV ?= yes

# Enable Linux spidev interface by default. We disable it on non-Linux targets.
CONFIG_LINUX_SPI ?= yes

# Disable wiki printing by default. It is only useful if you have wiki access.
CONFIG_PRINT_WIKI ?= no

# Support for reading a flashmap from a device tree in the image
CONFIG_FDTMAP ?= no

# Enable all features if CONFIG_EVERYTHING=yes is given
ifeq ($(CONFIG_EVERYTHING), yes)
$(foreach var, $(filter CONFIG_%, $(.VARIABLES)),\
	$(if $(filter no, $($(var))),\
		$(eval $(var)=yes)))
endif

# Bitbanging SPI infrastructure, default off unless needed.
ifeq ($(CONFIG_RAYER_SPI), yes)
override CONFIG_BITBANG_SPI = yes
else
ifeq ($(CONFIG_PONY_SPI), yes)
override CONFIG_BITBANG_SPI = yes
else
ifeq ($(CONFIG_INTERNAL), yes)
override CONFIG_BITBANG_SPI = yes
else
ifeq ($(CONFIG_NICINTEL_SPI), yes)
override CONFIG_BITBANG_SPI = yes
else
ifeq ($(CONFIG_OGP_SPI), yes)
override CONFIG_BITBANG_SPI = yes
else
CONFIG_BITBANG_SPI ?= no
endif
endif
endif
endif
endif

###############################################################################
# Handle CONFIG_* variables that depend on others set (and verified) above.

# The external DMI decoder (dmidecode) does not work in libpayload. Bail out if the internal one got disabled.
ifeq ($(TARGET_OS), libpayload)
ifeq ($(CONFIG_INTERNAL), yes)
ifeq ($(CONFIG_INTERNAL_DMI), no)
UNSUPPORTED_FEATURES += CONFIG_INTERNAL_DMI=no
else
override CONFIG_INTERNAL_DMI = yes
endif
endif
endif

ifeq ($(CONFIG_INTERNAL), yes)
FEATURE_CFLAGS += -D'CONFIG_INTERNAL=1'
PROGRAMMER_OBJS += processor_enable.o chipset_enable.o board_enable.o cbtable.o dmi.o internal.o cros_ec.o
ifeq ($(ARCH),x86)
PROGRAMMER_OBJS += cros_ec_lpc.o it87spi.o it85spi.o mec1308.o sb600spi.o wbsio_spi.o mcp6x_spi.o wpce775x.o ene_lpc.o
PROGRAMMER_OBJS += ichspi.o ich_descriptors.o
else
ifeq ($(ARCH),arm)
PROGRAMMER_OBJS += cros_ec_i2c.o
endif
NEED_PCI := yes
endif
endif

PROGRAMMER_OBJS += cros_ec_dev.o

ifeq ($(CONFIG_SERPROG), yes)
FEATURE_CFLAGS += -D'CONFIG_SERPROG=1'
PROGRAMMER_OBJS += serprog.o
NEED_SERIAL := yes
NEED_NET := yes
endif

ifeq ($(CONFIG_RAYER_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_RAYER_SPI=1'
PROGRAMMER_OBJS += rayer_spi.o
# Actually, NEED_PCI is wrong. NEED_IOPORT_ACCESS would be more correct.
NEED_PCI := yes
endif

ifeq ($(CONFIG_BITBANG_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_BITBANG_SPI=1'
PROGRAMMER_OBJS += bitbang_spi.o
endif

ifeq ($(CONFIG_NIC3COM), yes)
FEATURE_CFLAGS += -D'CONFIG_NIC3COM=1'
PROGRAMMER_OBJS += nic3com.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_GFXNVIDIA), yes)
FEATURE_CFLAGS += -D'CONFIG_GFXNVIDIA=1'
PROGRAMMER_OBJS += gfxnvidia.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_SATASII), yes)
FEATURE_CFLAGS += -D'CONFIG_SATASII=1'
PROGRAMMER_OBJS += satasii.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_ATAHPT), yes)
FEATURE_CFLAGS += -D'CONFIG_ATAHPT=1'
PROGRAMMER_OBJS += atahpt.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_FT2232_SPI), yes)
FTDILIBS := $(shell $(PKG_CONFIG) --libs libftdi1 2>/dev/null ||	\
		    $(PKG_CONFIG) --libs libftdi 2>/dev/null || echo "-lftdi -lusb")
FTDICFLAGS := $(shell $(PKG_CONFIG) --cflags libftdi1 2>/dev/null ||	\
		      $(PKG_CONFIG) --cflags libftdi 2>/dev/null)
# This is a totally ugly hack.
FEATURE_CFLAGS += $(shell LC_ALL=C grep -q "FTDISUPPORT := yes" .features && echo "$(FTDICFLAGS) -D'CONFIG_FT2232_SPI=1'")
FEATURE_LIBS += $(shell LC_ALL=C grep -q "FTDISUPPORT := yes" .features && echo "$(FTDILIBS)")
PROGRAMMER_OBJS += ft2232_spi.o
NEED_USB := yes
endif

ifeq ($(CONFIG_DUMMY), yes)
FEATURE_CFLAGS += -D'CONFIG_DUMMY=1'
PROGRAMMER_OBJS += dummyflasher.o
endif

ifeq ($(CONFIG_DRKAISER), yes)
FEATURE_CFLAGS += -D'CONFIG_DRKAISER=1'
PROGRAMMER_OBJS += drkaiser.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_NICREALTEK), yes)
FEATURE_CFLAGS += -D'CONFIG_NICREALTEK=1'
PROGRAMMER_OBJS += nicrealtek.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_NICNATSEMI), yes)
FEATURE_CFLAGS += -D'CONFIG_NICNATSEMI=1'
PROGRAMMER_OBJS += nicnatsemi.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_NICINTEL), yes)
FEATURE_CFLAGS += -D'CONFIG_NICINTEL=1'
PROGRAMMER_OBJS += nicintel.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_NICINTEL_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_NICINTEL_SPI=1'
PROGRAMMER_OBJS += nicintel_spi.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_OGP_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_OGP_SPI=1'
PROGRAMMER_OBJS += ogp_spi.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_BUSPIRATE_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_BUSPIRATE_SPI=1'
PROGRAMMER_OBJS += buspirate_spi.o
NEED_SERIAL := yes
endif

ifeq ($(CONFIG_RAIDEN_DEBUG_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_RAIDEN_DEBUG_SPI=1'
PROGRAMMER_OBJS += raiden_debug_spi.o usb_device.o
NEED_USB := yes
endif

ifeq ($(CONFIG_LINUX_I2C), yes)
FEATURE_CFLAGS += -D'CONFIG_LINUX_I2C=1'
PROGRAMMER_OBJS += linux_i2c.o
endif

ifeq ($(CONFIG_LINUX_MTD), yes)
FEATURE_CFLAGS += -D'CONFIG_LINUX_MTD=1'
PROGRAMMER_OBJS += linux_mtd.o
endif

ifeq ($(CONFIG_DEDIPROG), yes)
FEATURE_CFLAGS += -D'CONFIG_DEDIPROG=1'
FEATURE_LIBS += -lusb
PROGRAMMER_OBJS += dediprog.o
endif

ifeq ($(CONFIG_SATAMV), yes)
FEATURE_CFLAGS += -D'CONFIG_SATAMV=1'
PROGRAMMER_OBJS += satamv.o
NEED_PCI := yes
endif

ifeq ($(CONFIG_LINUX_SPI), yes)
FEATURE_CFLAGS += -D'CONFIG_LINUX_SPI=1'
PROGRAMMER_OBJS += linux_spi.o
endif

ifeq ($(NEED_SERIAL), yes)
LIB_OBJS += serial.o
endif

ifeq ($(NEED_NET), yes)
ifeq ($(TARGET_OS), SunOS)
LIBS += -lsocket -lnsl
endif
endif

ifeq ($(NEED_PCI), yes)
CHECK_LIBPCI = yes
FEATURE_CFLAGS += -D'NEED_PCI=1'
PROGRAMMER_OBJS += pcidev.o physmap.o hwaccess.o
ifeq ($(TARGET_OS), NetBSD)
# The libpci we want is called libpciutils on NetBSD and needs NetBSD libpci.
LIBS += -lpciutils -lpci
# For (i386|x86_64)_iopl(2).
LIBS += -l$(shell uname -p)
else
ifeq ($(TARGET_OS), DOS)
# FIXME There needs to be a better way to do this
LIBS += ../libpci/lib/libpci.a
else
ifeq ($(CONFIG_STATIC), yes)
LIBS_PCI := $(shell $(PKG_CONFIG) --libs --static libpci)
LIBS += -static $(LIBS_PCI)
else
LIBS += -lpci
endif
ifeq ($(TARGET_OS), OpenBSD)
# For (i386|amd64)_iopl(2).
LIBS += -l$(shell uname -m)
endif
endif
endif
endif

ifeq ($(NEED_USB),yes)
FEATURE_CFLAGS += $(shell $(PKG_CONFIG) --cflags libusb-1.0)
FEATURE_LIBS   += $(shell $(PKG_CONFIG) --libs libusb-1.0)
endif

ifeq ($(CONFIG_PRINT_WIKI), yes)
FEATURE_CFLAGS += -D'CONFIG_PRINT_WIKI=1'
CLI_OBJS += print_wiki.o
endif

ifeq ($(CONFIG_USE_OS_TIMER), yes)
FEATURE_CFLAGS += -D'CONFIG_USE_OS_TIMER=1'
else
FEATURE_CFLAGS += -D'CONFIG_USE_OS_TIMER=0'
endif

FEATURE_CFLAGS += $(call debug_shell,grep -q "UTSNAME := yes" .features && printf "%s" "-D'HAVE_UTSNAME=1'")

# We could use PULLED_IN_LIBS, but that would be ugly.
FEATURE_LIBS += $(call debug_shell,grep -q "NEEDLIBZ := yes" .libdeps && printf "%s" "-lz")

LIBFLASHROM_OBJS = $(CHIP_OBJS) $(PROGRAMMER_OBJS) $(LIB_OBJS)
OBJS = $(CLI_OBJS) $(LIBFLASHROM_OBJS)

$(PROGRAM)$(EXEC_SUFFIX): $(OBJS)
	$(CC) $(LDFLAGS) -o $(PROGRAM)$(EXEC_SUFFIX) $(OBJS) $(FEATURE_LIBS) $(LIBS)

libflashrom.a: $(LIBFLASHROM_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# TAROPTIONS reduces information leakage from the packager's system.
# If other tar programs support command line arguments for setting uid/gid of
# stored files, they can be handled here as well.
TAROPTIONS = $(shell LC_ALL=C tar --version|grep -q GNU && echo "--owner=root --group=root")

%.o: %.c .features
	$(CC) -MMD $(CFLAGS) $(CPPFLAGS) $(FEATURE_CFLAGS) $(SVNDEF) -o $@ -c $<

# Make sure to add all names of generated binaries here.
# This includes all frontends and libflashrom.
# We don't use EXEC_SUFFIX here because we want to clean everything.
clean:
	rm -f $(PROGRAM) $(PROGRAM).exe libflashrom.a *.o *.d $(PROGRAM).8 $(PROGRAM).8.html $(BUILD_DETAILS_FILE)
	@+$(MAKE) -C util/ich_descriptors_tool/ clean

distclean: clean
	rm -f .features .libdeps

strip: $(PROGRAM)$(EXEC_SUFFIX)
	$(STRIP) $(STRIP_ARGS) $(PROGRAM)$(EXEC_SUFFIX)

# to define test programs we use verbatim variables, which get exported
# to environment variables and are referenced with $$<varname> later

define COMPILER_TEST
int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	return 0;
}
endef
export COMPILER_TEST

compiler: featuresavailable
	@printf "Checking for a C compiler... " | tee -a $(BUILD_DETAILS_FILE)
	@echo "$$COMPILER_TEST" > .test.c
	@printf "\nexec: %s\n" "$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) .test.c -o .test$(EXEC_SUFFIX)" >>$(BUILD_DETAILS_FILE)
	@{ { { { { $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) .test.c -o .test$(EXEC_SUFFIX) >&2 && \
		echo "found." || { echo "not found."; \
		rm -f .test.c .test$(EXEC_SUFFIX); exit 1; }; } 2>>$(BUILD_DETAILS_FILE); echo $? >&3 ; } | tee -a $(BUILD_DETAILS_FILE) >&4; } 3>&1;} | { read rc ; exit ${rc}; } } 4>&1
	@rm -f .test.c .test$(EXEC_SUFFIX)
	@printf "Target arch is "
	@# FreeBSD wc will output extraneous whitespace.
	@echo $(ARCH)|wc -w|grep -q '^[[:blank:]]*1[[:blank:]]*$$' ||	\
		( echo "unknown. Aborting."; exit 1)
	@printf "%s\n" '$(ARCH)'
	@printf "Target OS is "
	@# FreeBSD wc will output extraneous whitespace.
	@echo $(TARGET_OS)|wc -w|grep -q '^[[:blank:]]*1[[:blank:]]*$$' ||	\
		( echo "unknown. Aborting."; exit 1)
	@printf "%s\n" '$(TARGET_OS)'
ifeq ($(TARGET_OS), libpayload)
	@$(CC) --version 2>&1 | grep -q coreboot || \
		( echo "Warning: It seems you are not using coreboot's reference compiler."; \
		  echo "This might work but usually does not, please beware." )
endif

define LIBPCI_TEST
/* Avoid a failing test due to libpci header symbol shadowing breakage */
#define index shadow_workaround_index
#if !defined __NetBSD__
#include <pci/pci.h>
#else
#include <pciutils/pci.h>
#endif
struct pci_access *pacc;
int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	pacc = pci_alloc();
	return 0;
}
endef
export LIBPCI_TEST

ifeq ($(CHECK_LIBPCI), yes)
pciutils: compiler
	@printf "Checking for libpci headers... " | tee -a $(BUILD_DETAILS_FILE)
	@echo "$$LIBPCI_TEST" > .test.c
	@$(CC) -c $(CPPFLAGS) $(CFLAGS) .test.c -o .test.o >/dev/null 2>&1 &&		\
		echo "found." || ( echo "not found."; echo;			\
		echo "Please install libpci headers (package pciutils-devel).";	\
		echo "See README for more information."; echo;			\
		rm -f .test.c .test.o; exit 1)
	@printf "Checking if libpci is present and sufficient... "
	@printf "" > .libdeps
	@$(CC) $(LDFLAGS) .test.o -o .test$(EXEC_SUFFIX) $(LIBS) >/dev/null 2>&1 &&				\
		echo "yes." || ( echo "no.";							\
		printf "Checking if libz+libpci are present and sufficient...";	\
		$(CC) $(LDFLAGS) .test.o -o .test$(EXEC_SUFFIX) $(LIBS) -lz >/dev/null 2>&1 &&		\
		( echo "yes."; echo "NEEDLIBZ := yes" > .libdeps ) || ( echo "no."; echo;	\
		echo "Please install libpci (package pciutils) and/or libz.";			\
		echo "See README for more information."; echo;				\
		rm -f .test.c .test.o .test$(EXEC_SUFFIX); exit 1) )
	@rm -f .test.c .test.o .test$(EXEC_SUFFIX)
else
pciutils: compiler
	@printf "" > .libdeps
endif

.features: features

# If a user does not explicitly request a non-working feature, we should
# silently disable it. However, if a non-working (does not compile) feature
# is explicitly requested, we should bail out with a descriptive error message.
featuresavailable:
ifneq ($(UNSUPPORTED_FEATURES), )
	@echo "The following features are unavailable on your machine: $(UNSUPPORTED_FEATURES)"
	@false
endif

define FTDI_TEST
#include <stdlib.h>
#include <ftdi.h>
struct ftdi_context *ftdic = NULL;
int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	return ftdi_init(ftdic);
}
endef
export FTDI_TEST

define UTSNAME_TEST
#include <sys/utsname.h>
struct utsname osinfo;
int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	uname (&osinfo);
	return 0;
}
endef
export UTSNAME_TEST

features: compiler
	@echo "FEATURES := yes" > .features.tmp
ifeq ($(CONFIG_FT2232_SPI), yes)
	@printf "Checking for FTDI support... " | tee -a $(BUILD_DETAILS_FILE)
	@echo "$$FTDI_TEST" > .featuretest.c
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(FTDICFLAGS) $(LDFLAGS) .featuretest.c -o .featuretest$(EXEC_SUFFIX) $(FTDILIBS) $(LIBS) >/dev/null 2>&1 &&	\
		( echo "found."; echo "FTDISUPPORT := yes" >> .features.tmp ) ||	\
		( echo "not found."; echo "FTDISUPPORT := no" >> .features.tmp )
endif
	@printf "Checking for utsname support... " | tee -a $(BUILD_DETAILS_FILE)
	@echo "$$UTSNAME_TEST" > .featuretest.c
	@printf "\nexec: %s\n" "$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) .featuretest.c -o .featuretest$(EXEC_SUFFIX)" >>$(BUILD_DETAILS_FILE)
	@ { $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) .featuretest.c -o .featuretest$(EXEC_SUFFIX) >&2 && \
		( echo "found."; echo "UTSNAME := yes" >> .features.tmp ) ||	\
		( echo "not found."; echo "UTSNAME := no" >> .features.tmp ) } 2>>$(BUILD_DETAILS_FILE) | tee -a $(BUILD_DETAILS_FILE)
	@$(DIFF) -q .features.tmp .features >/dev/null 2>&1 && rm .features.tmp || mv .features.tmp .features
	@rm -f .featuretest.c .featuretest$(EXEC_SUFFIX)

$(PROGRAM).8.html: $(PROGRAM).8
	@groff -mandoc -Thtml $< >$@

MAN_DATE := $(shell ./util/getrevision.sh -d $(PROGRAM).8.tmpl 2>/dev/null)
$(PROGRAM).8: $(PROGRAM).8.tmpl
	@# Add the man page change date and version to the man page
	@sed -e 's#.TH FLASHROM 8 .*#.TH FLASHROM 8 "$(MAN_DATE)" "$(VERSION)" "$(MAN_DATE)"#' <$< >$@

install: $(PROGRAM)$(EXEC_SUFFIX) $(PROGRAM).8
	mkdir -p $(DESTDIR)$(PREFIX)/sbin
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	$(INSTALL) -m 0755 $(PROGRAM)$(EXEC_SUFFIX) $(DESTDIR)$(PREFIX)/sbin
	$(INSTALL) -m 0644 $(PROGRAM).8 $(DESTDIR)$(MANDIR)/man8

_export: $(PROGRAM).8
	@rm -rf "$(EXPORTDIR)/flashrom-$(RELEASENAME)"
	@mkdir -p "$(EXPORTDIR)/flashrom-$(RELEASENAME)"
	@git archive HEAD | tar -x -C "$(EXPORTDIR)/flashrom-$(RELEASENAME)"
	@sed	-e  's/^VERSION :=.*/VERSION := $(VERSION)/' \
		-e 's/^MAN_DATE :=.*/MAN_DATE := $(MAN_DATE)/' \
		-e 's#./util/getrevision.sh -c#false#' \
		Makefile >"$(EXPORTDIR)/flashrom-$(RELEASENAME)/Makefile"
#	Restore modification date of all tracked files not marked 'export-ignore' in .gitattributes.
#	sed is required to filter out file names having the attribute set.
	@git ls-tree -r -z -t --full-name --name-only HEAD | \
		git check-attr -z --stdin export-ignore | \
		sed -zne 'x;n;n;s/^set$$//;t;x;p' | \
		xargs -0 sh -c 'for f; do \
			touch -d $$(git log --pretty=format:%cI -1 HEAD -- "$$f") \
				"$(EXPORTDIR)/flashrom-$(RELEASENAME)/$$f"; \
		done'

export: _export
	@echo "Exported $(EXPORTDIR)/flashrom-$(RELEASENAME)/"

tarball: _export
	@tar -cz --format=ustar -f $(EXPORTDIR)/flashrom-$(RELEASENAME).tar.gz -C $(EXPORTDIR)/ \
		$(TAROPTIONS) flashrom-$(RELEASENAME)/
#	Delete the exported directory again because it is most likely what's expected by the user.
	@rm -rf $(EXPORTDIR)/flashrom-$(RELEASENAME)
	@echo Created $(EXPORTDIR)/flashrom-$(RELEASENAME).tar.gz

libpayload: clean
	make CC="CC=i386-elf-gcc lpgcc" AR=i386-elf-ar RANLIB=i386-elf-ranlib

.PHONY: all install clean distclean compiler hwlibs features _export export tarball featuresavailable libpayload

# Disable implicit suffixes and built-in rules (for performance and profit)
.SUFFIXES:

-include $(OBJS:.o=.d)
