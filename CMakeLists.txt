cmake_minimum_required(VERSION 3.9)
project(flashrom_google)

# Using C89/C90 standard, treat warnings as errors
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_FLAGS "-Werror ")

# Basic source objects
set(FLASHROM_CHIP_OBJS "jedec.c stm50flw0x0x.c w39.c w29ee011.c
	                    sst28sf040.c m29f400bt.c 82802ab.c pm49fl00x.c
	                    sst49lfxxxc.c sst_fwhub.c flashchips.c flashchips.h spi.o spi25.o sharplhf00l04.o
	                    a25.o at25.o s25f.o spi4ba.o opaque.o writeprotect.o")

set(FLASHROM_LIB_OBJS "android.o layout.o file.o fmap.o search.o")

set(FLASHROM_LOCK_OBJS "big_lock.o file_lock.o")

set(FLASHROM_CLI_OBJS "flashrom.o cli_mfg.o cli_output.o print.o")

set(FLASHROM_PROGRAMMERS_OBJS "udelay.o programmer.o")

# Flashrom Version
set(FLASHROM_VERSION "1.0.0")

# Is it built for x86 target?
set(FLASHROM_FOR_X86 FALSE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(FLASHROM_BUILD_AT_X86 TRUE)
else()
    set(FLASHROM_BUILD_AT_X86 FALSE)
endif()


###############################################################################
# Default settings of CONFIG_* variables.

# Always enable internal/onboard support for now.
OPTION(CONFIG_INTERNAL "enable internal/onboard support" ON)

# Always enable serprog for now.
OPTION(CONFIG_SERPROG "enable serprog" ON)

# RayeR SPIPGM hardware support
OPTION(CONFIG_RAYER_SPI "enable RayeR SPIPGM" ON)

# Always enable 3Com NICs for now.
OPTION(CONFIG_NIC3COM "enable 3Com NICs" ON)

# Enable NVIDIA graphics cards. Note: write and erase do not work properly.
OPTION(CONFIG_GFXNVIDIA "enable NVIDIA graphic cards (read-only support)" ON)

# Always enable SiI SATA controllers for now.
OPTION(CONFIG_SATASII "enable SiI SATA controllers" ON)

# Highpoint (HPT) ATA/RAID controller support.
# IMPORTANT: This code is not yet working!
OPTION(CONFIG_ATAHPT "enable Highpoint (HPT) ATA/RAID controller support" OFF)

OPTION(CONFIG_ATAVIA "enable VIA SATA controllers support" OFF)

OPTION(CONFIG_ATAPROMISE "" OFF)

# Always enable FT2232 SPI dongles for now.
OPTION(CONFIG_FT2232_SPI "enable FT2232 SPI dongles" ON)

# Always enable dummy tracing for now.
OPTION(CONFIG_DUMMY "enable dummy tracing device" ON)

# Always enable Dr. Kaiser for now.
OPTION(CONFIG_DRKAISER "enable Dr. Kaiser" ON)

# Always enable Realtek NICs for now.
OPTION(CONFIG_NICREALTEK "enable Realtek NICs" ON)

# Disable National Semiconductor NICs until support is complete and tested.
OPTION(CONFIG_NICNATSEMI "enable National Semiconductor NICs support (not complete)" OFF)

# Always enable Intel NICs for now.
OPTION(CONFIG_NICINTEL "enable Intel NICs support" ON)

# Always enable SPI on Intel NICs for now.
OPTION(CONFIG_NICINTEL_SPI "enable SPI on Intel NICs support" ON)

# Always enable SPI on OGP cards for now.
OPTION(CONFIG_OGP_SPI "enable SPI on OGP cards" ON)

# Always enable Bus Pirate SPI for now.
OPTION(CONFIG_BUSPIRATE_SPI "enable Bus Pirate SPI" ON)

# Raiden Debug SPI-over-USB support.
OPTION(CONFIG_RAIDEN_DEBUG_SPI "enable Raiden Debug SPI-over-USB support" OFF)

# Enable Linux I2C for ChromeOS EC
OPTION(CONFIG_LINUX_I2C "enable Linux I2C support" OFF)
OPTION(CONFIG_LINUX_MTD "enable Linux MTD support" OFF)

# Always enable Dediprog SF100 for now.
OPTION(CONFIG_DEDIPROG "enable Dediprog SF100 support" ON)

# Always enable Marvell SATA controllers for now.
OPTION(CONFIG_SATAMV "enable Marvell SATA controllers" ON)

# Enable Linux spidev interface by default. We disable it on non-Linux targets.
OPTION(CONFIG_LINUX_SPI "enable Linux SPI support (Linux target only)" ON)

# Disable wiki printing by default. It is only useful if you have wiki access.
OPTION(CONFIG_PRINT_WIKI "enable Wiki printing" ON)

# Support for reading a flashmap from a device tree in the image
OPTION(CONFIG_FDTMAP "enable Flashmap from a device tree support" OFF)

# Bitbang SPI
OPTION(CONFIG_BITBANG_SPI "enable Bitbang SPI" ON)

# Add include path for some Unix OSes
if(APPLE)
    include_directories(AFTER "/opt/local/include")
    include_directories(AFTER "/usr/local/include")
    link_directories(AFTER "/usr/local/lib")
    link_directories(AFTER "/opt/local/lib")
elseif(UNIX)
    include_directories(AFTER "/usr/local/include")
    link_directories(AFTER "/usr/local/lib")
endif()

if(MINGW AND MSYS OR NOT FLASHROM_FOR_X86)
    set(CONFIG_LINUX_SPI OFF CACHE BOOL "")
    set(CONFIG_MSTARDDC_SPI OFF CACHE BOOL "")
    set(CONFIG_RAYER_SPI OFF CACHE BOOL "")
    set(CONFIG_NIC3COM OFF CACHE BOOL "")
    set(CONFIG_GFXNVIDIA OFF CACHE BOOL "")
    set(CONFIG_SATASII OFF CACHE BOOL "")
    set(CONFIG_ATAHPT OFF CACHE BOOL "")
    set(CONFIG_ATAVIA OFF CACHE BOOL "")
    set(CONFIG_ATAPROMISE OFF CACHE BOOL "")
    set(CONFIG_IT8212 OFF CACHE BOOL "")
    set(CONFIG_DRKAISER OFF CACHE BOOL "")
    set(CONFIG_NICREALTEK OFF CACHE BOOL "")
    set(CONFIG_NICNATSEMI OFF CACHE BOOL "")
    set(CONFIG_NICINTEL OFF CACHE BOOL "")
    set(CONFIG_NICINTEL_EEPROM OFF CACHE BOOL "")
    set(CONFIG_NICINTEL_SPI OFF CACHE BOOL "")
    set(CONFIG_OGP_SPI OFF CACHE BOOL "")
    set(CONFIG_SATAMV OFF CACHE BOOL "")
    set(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} "-Dffs=__builtin_ffs -D__USE_MINGW_ANSI_STDIO=1")
endif()

# Android target doesn't support Rayer SPI
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(CONFIG_RAYER_SPI OFF CACHE BOOL "")
endif()

# Set the build date (doesn't work???)
string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%dT-%H%M%S")
set(FLASHROM_VERSION ${FLASHROM_VERSION}-${BUILD_TIMESTAMP})
add_definitions(-DFLASHROM_VERSION=${FLASHROM_VERSION})

# Disable Bitbang SPI if it's not necessary
if(NOT CONFIG_RAYER_SPI
        AND NOT CONFIG_PONY_SPI
        AND NOT CONFIG_INTERNAL
        AND NOT CONFIG_NICINTEL_SPI
        AND NOT CONFIG_OGP_SPI)
    set(CONFIG_BITBANG_SPI OFF CACHE BOOL "")
endif()

#################################################################################
# Parse build arguments
#################################################################################

# Internal programmers
if(CONFIG_INTERNAL)
    add_definitions(-D'CONFIG_INTERNAL=1')
    set(FLASHROM_PROGRAMMERS_OBJS
            ${FLASHROM_PROGRAMMERS_OBJS}
            "processor_enable.o chipset_enable.o board_enable.o cbtable.o dmi.o internal.o")

    if(FLASHROM_FOR_X86)
        set(FLASHROM_PROGRAMMERS_OBJS
                ${FLASHROM_PROGRAMMERS_OBJS}
                "it87spi.o it85spi.o mec1308.o sb600spi.o wbsio_spi.o mcp6x_spi.o wpce775x.o ene_lpc.o")
        set(FLASHROM_PROGRAMMERS_OBJS
                ${FLASHROM_PROGRAMMERS_OBJS}
                "ichspi.o ich_descriptors.o amd_imc.o")
    endif()
endif()

# Serial programmers
if(CONFIG_SERPROG)
    add_definitions(-D'CONFIG_SERPROG=1')


add_executable(flashrom
        82802ab.c
        a25.c
        amd_imc.c
        android.c
        android.h
        archtest.c
        at25.c
        atahpt.c
        big_lock.c
        big_lock.h
        bitbang_spi.c
        board_enable.c
        buspirate_spi.c
        cbtable.c
        check.h
        chipdrivers.h
        chipset_enable.c
        cli_classic.c
        cli_mfg.c
        cli_output.c
        coreboot_tables.h
        dediprog.c
        dmi.c
        drkaiser.c
        dummyflasher.c
        ene_lpc.c
        fdtmap.c
        fdtmap.h
        file.c
        file.h
        file_lock.c
        flash.h
        flashchips.c
        flashchips.h
        flashrom.c
        fmap.c
        fmap.h
        ft2232_spi.c
        gfxnvidia.c
        hwaccess.c
        hwaccess.h
        ich_descriptors.c
        ich_descriptors.h
        ichspi.c
        internal.c
        ipc_lock.h
        it85spi.c
        it87spi.c
        jedec.c
        layout.c
        layout.h
        linux_i2c.c
        linux_mtd.c
        linux_spi.c
        locks.h
        m29f400bt.c
        mcp6x_spi.c
        mec1308.c
        nic3com.c
        nicintel.c
        nicintel_spi.c
        nicnatsemi.c
        nicrealtek.c
        ogp_spi.c
        opaque.c
        os.h
        pcidev.c
        physmap.c
        platform.h
        pm49fl00x.c
        print.c
        print_wiki.c
        processor_enable.c
        programmer.c
        programmer.h
        raiden_debug_spi.c
        rayer_spi.c
        s25f.c
        satamv.c
        satasii.c
        sb600spi.c
        search.c
        search.h
        serial.c
        serprog.c
        serprog.h
        sharplhf00l04.c
        spi.c
        spi.h
        spi25.c
        spi4ba.c
        spi4ba.h
        sst28sf040.c
        sst49lfxxxc.c
        sst_fwhub.c
        stm50flw0x0x.c
        udelay.c
        usb_device.c
        usb_device.h
        w29ee011.c
        w39.c
        wbsio_spi.c
        wpce775x.c
        writeprotect.c
        writeprotect.h)
