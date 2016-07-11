/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2004 Tyan Corp <yhlu@tyan.com>
 * Copyright (C) 2005-2008 coresystems GmbH
 * Copyright (C) 2008,2009,2010 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include "big_lock.h"
#include "flash.h"
#include "flashchips.h"
#include "power.h"
#include "programmer.h"
#include "writeprotect.h"

#define LOCK_TIMEOUT_SECS	180

/* This variable is shared with doit() in flashrom.c */
int set_ignore_fmap = 0;
int set_ignore_lock = 0;

#if CONFIG_INTERNAL == 1
static enum programmer default_programmer = PROGRAMMER_INTERNAL;
#elif CONFIG_DUMMY == 1
static enum programmer default_programmer = PROGRAMMER_DUMMY;
#else
/* If neither internal nor dummy are selected, we must pick a sensible default.
 * Since there is no reason to prefer a particular external programmer, we fail
 * if more than one of them is selected. If only one is selected, it is clear
 * that the user wants that one to become the default.
 */
#if CONFIG_NIC3COM+CONFIG_NICREALTEK+CONFIG_NICNATSEMI+CONFIG_GFXNVIDIA+CONFIG_DRKAISER+CONFIG_SATASII+CONFIG_ATAHPT+CONFIG_FT2232_SPI+CONFIG_SERPROG+CONFIG_BUSPIRATE_SPI+CONFIG_DEDIPROG+CONFIG_RAYER_SPI+CONFIG_NICINTEL+CONFIG_NICINTEL_SPI+CONFIG_OGP_SPI+CONFIG_SATAMV > 1
#error Please enable either CONFIG_DUMMY or CONFIG_INTERNAL or disable support for all programmers except one.
#endif
static enum programmer default_programmer =
#if CONFIG_NIC3COM == 1
	PROGRAMMER_NIC3COM
#endif
#if CONFIG_NICREALTEK == 1
	PROGRAMMER_NICREALTEK
#endif
#if CONFIG_NICNATSEMI == 1
	PROGRAMMER_NICNATSEMI
#endif
#if CONFIG_GFXNVIDIA == 1
	PROGRAMMER_GFXNVIDIA
#endif
#if CONFIG_DRKAISER == 1
	PROGRAMMER_DRKAISER
#endif
#if CONFIG_SATASII == 1
	PROGRAMMER_SATASII
#endif
#if CONFIG_ATAHPT == 1
	PROGRAMMER_ATAHPT
#endif
#if CONFIG_FT2232_SPI == 1
	PROGRAMMER_FT2232_SPI
#endif
#if CONFIG_SERPROG == 1
	PROGRAMMER_SERPROG
#endif
#if CONFIG_BUSPIRATE_SPI == 1
	PROGRAMMER_BUSPIRATE_SPI
#endif
#if CONFIG_DEDIPROG == 1
	PROGRAMMER_DEDIPROG
#endif
#if CONFIG_RAYER_SPI == 1
	PROGRAMMER_RAYER_SPI
#endif
#if CONFIG_NICINTEL == 1
	PROGRAMMER_NICINTEL
#endif
#if CONFIG_NICINTEL_SPI == 1
	PROGRAMMER_NICINTEL_SPI
#endif
#if CONFIG_OGP_SPI == 1
	PROGRAMMER_OGP_SPI
#endif
#if CONFIG_SATAMV == 1
	PROGRAMMER_SATAMV
#endif
#if CONFIG_LINUX_MTD == 1
	PROGRAMMER_LINUX_MTD
#endif
#if CONFIG_LINUX_SPI == 1
	PROGRAMMER_LINUX_SPI
#endif
;
#endif

void cli_mfg_usage(const char *name)
{

	msg_ginfo("Usage: flashrom [-n] [-V] [-f] [-h|-R|-L|"
#if CONFIG_PRINT_WIKI == 1
	         "-z|"
#endif
	         "-E|-r <file>|-w <file>|-v <file>]\n"
	       "       [-i <image>[:<file>]] [-c <chipname>] "
	               "[-m [<vendor>:]<part>] [-l <file>]\n"
	       "       [-p <programmer>[:<parameters>]]\n\n");

	msg_ginfo("Please note that the command line interface for flashrom has "
	         "changed between\n"
	       "0.9.1 and 0.9.2 and will change again before flashrom 1.0.\n"
	       "Do not use flashrom in scripts or other automated tools "
	         "without checking\n"
	       "that your flashrom version won't interpret options in a "
	         "different way.\n\n");

	msg_ginfo("   -h | --help                       print this help text\n"
	       "   -R | --version                    print version (release)\n"
	       "   -r | --read <file|->              read flash and save to "
	         "<file> or write on the standard output\n"
	       "   -w | --write <file|->             write <file> or "
	         "the content provided on the standard input to flash\n"
	       "   -v | --verify <file|->            verify flash against "
	         "<file> or the content provided on the standard input\n"
	       "   -E | --erase                      erase flash device\n"
	       "   -V | --verbose                    more verbose output\n"
	       "   -c | --chip <chipname>            probe only for specified "
	         "flash chip\n"
#if CONFIG_INTERNAL == 1
	       /* FIXME: --mainboard should be a programmer parameter */
	       "   -m | --mainboard <[vendor:]part>  override mainboard "
	         "detection\n"
#endif
	       "   -f | --force                      force specific operations "
	         "(see man page)\n"
	       "   -n | --noverify                   don't auto-verify\n"
	       "   -l | --layout <file>              read ROM layout from "
	         "<file>\n"
	       "   -i | --image <name>[:<file>]      only access image <name> "
	         "from flash layout\n"
	       "   -L | --list-supported             print supported devices\n"
	       "   -x | --extract                    extract regions to files\n"
#if CONFIG_PRINT_WIKI == 1
	       "   -z | --list-supported-wiki        print supported devices "
	         "in wiki syntax\n"
#endif
	       "   -b | --broken-timers              assume system timers are "
	         "broken\n"
	       "   -p | --programmer <name>[:<param>] specify the programmer "
	         "device\n"
	);

	list_programmers_linebreak(37, 80, 1);

	msg_ginfo("Long-options:\n"
	       "   --diff <file>                     diff from file instead of ROM\n"
	       "   --fast-verify                     only verify -i part\n"
	       "   --flash-name                      flash vendor and device name\n"
	       "   --get-size                        get chip size (bytes)\n"
	       "   --ignore-fmap                     ignore fmap structure\n"
	       "   --ignore-lock                     do not acquire big lock\n"
	       "   --wp-disable                      disable write protection\n"
	       "   --wp-enable                       enable write protection\n"
	       "   --wp-list                         list write protection ranges\n"
	       "   --wp-range <start> <length>       set write protect range\n"
	       "   --wp-status                       show write protect status\n"
	       );

	msg_ginfo("\nYou can specify one of -h, -R, -L, "
#if CONFIG_PRINT_WIKI == 1
	         "-z, "
#endif
	         "-E, -r, -w, -v or no operation.\n"
	       "If no operation is specified, flashrom will only probe for "
	         "flash chips.\n\n");
}

void cli_mfg_abort_usage(const char *name)
{
	msg_gerr("Please run \"%s --help\" for usage info.\n", name);
	exit(1);
}

enum LONGOPT_RETURN_VALUES {
	/* start after ASCII chars */
	LONGOPT_GET_SIZE = 256,
	LONGOPT_DIFF,
	LONGOPT_FLASH_NAME,
	LONGOPT_WP_STATUS,
	LONGOPT_WP_SET_RANGE,
	LONGOPT_WP_ENABLE,
	LONGOPT_WP_DISABLE,
	LONGOPT_WP_LIST,
	LONGOPT_IGNORE_FMAP,
	LONGOPT_FAST_VERIFY,
	LONGOPT_IGNORE_LOCK,
};

int main(int argc, char *argv[])
{
	unsigned long size;
	/* Probe for up to three flash chips. */
	const struct flashchip *flash;
	struct flashctx flashes[3];
	struct flashctx *fill_flash = NULL;
	int startchip = 0;
	int chipcount = 0;
	const char *name;
	int namelen;
	int opt;
	int option_index = 0;
	int force = 0;
	int read_it = 0, write_it = 0, erase_it = 0, verify_it = 0,
	    get_size = 0, set_wp_range = 0, set_wp_enable = 0,
	    set_wp_disable = 0, wp_status = 0, wp_list = 0, flash_name = 0;
	int dont_verify_it = 0, list_supported = 0, extract_it = 0;
#if CONFIG_PRINT_WIKI == 1
	int list_supported_wiki = 0;
#endif
	int operation_specified = 0;
	int i, j;
	enum programmer prog = PROGRAMMER_INVALID;
	int rc = 0;
	int found_chip = 0;

	const char *optstring = "rRwvnVEfc:m:l:i:p:Lzhbx";
	static struct option long_options[] = {
		{"read", 0, 0, 'r'},
		{"write", 0, 0, 'w'},
		{"erase", 0, 0, 'E'},
		{"verify", 0, 0, 'v'},
		{"noverify", 0, 0, 'n'},
		{"chip", 1, 0, 'c'},
		{"mainboard", 1, 0, 'm'},
		{"verbose", 0, 0, 'V'},
		{"force", 0, 0, 'f'},
		{"layout", 1, 0, 'l'},
		{"image", 1, 0, 'i'},
		{"list-supported", 0, 0, 'L'},
		{"list-supported-wiki", 0, 0, 'z'},
		{"extract", 0, 0, 'x'},
		{"programmer", 1, 0, 'p'},
		{"help", 0, 0, 'h'},
		{"version", 0, 0, 'R'},
		{"get-size", 0, 0, LONGOPT_GET_SIZE},
		{"flash-name", 0, 0, LONGOPT_FLASH_NAME},
		{"diff", 1, 0, LONGOPT_DIFF},
		{"wp-status", 0, 0, LONGOPT_WP_STATUS},
		{"wp-range", 0, 0, LONGOPT_WP_SET_RANGE},
		{"wp-enable", optional_argument, 0, LONGOPT_WP_ENABLE},
		{"wp-disable", 0, 0, LONGOPT_WP_DISABLE},
		{"wp-list", 0, 0, LONGOPT_WP_LIST},
		{"broken-timers", 0, 0, 'b' },
		{"ignore-fmap", 0, 0, LONGOPT_IGNORE_FMAP},
		{"fast-verify", 0, 0, LONGOPT_FAST_VERIFY},
		{"ignore-lock", 0, 0, LONGOPT_IGNORE_LOCK},
		{0, 0, 0, 0}
	};

	char *filename = NULL;
	char *diff_file = NULL;

	char *tempstr = NULL;
	char *pparam = NULL;
	char *wp_mode_opt = NULL;

	print_version();

	if (selfcheck())
		exit(1);

	setbuf(stdout, NULL);
	/* FIXME: Delay all operation_specified checks until after command
	 * line parsing to allow --help overriding everything else.
	 */
	while ((opt = getopt_long(argc, argv, optstring,
				  long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'r':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			read_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'w':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			write_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'v':
			//FIXME: gracefully handle superfluous -v
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			if (dont_verify_it) {
				msg_gerr("--verify and --noverify are"
					"mutually exclusive. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			if (!verify_it) verify_it = VERIFY_FULL;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'n':
			if (verify_it) {
				msg_gerr("--verify and --noverify are"
					"mutually exclusive. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			dont_verify_it = 1;
			break;
		case 'c':
			chip_to_probe = strdup(optarg);
			break;
		case 'V':
			verbose++;
			break;
		case 'E':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			erase_it = 1;
#if CONFIG_USE_OS_TIMER == 0
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
#endif
			break;
		case 'm':
#if CONFIG_INTERNAL == 1
			tempstr = strdup(optarg);
			lb_vendor_dev_from_string(tempstr);
#else
			msg_gerr("Error: Internal programmer support "
				"was not compiled in and --mainboard only\n"
				"applies to the internal programmer. Aborting.\n");
			cli_mfg_abort_usage(argv[0]);
#endif
			break;
		case 'f':
			force = 1;
			break;
		case 'l':
			tempstr = strdup(optarg);
			if (read_romlayout(tempstr))
				cli_mfg_abort_usage(argv[0]);
			break;
		case 'i':
			tempstr = strdup(optarg);
			if (register_include_arg(tempstr) < 0)
				exit(1);
			break;
		case 'L':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			list_supported = 1;
			break;
		case 'x':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			extract_it = 1;
			break;
		case 'z':
#if CONFIG_PRINT_WIKI == 1
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			list_supported_wiki = 1;
#else
			msg_gerr("Error: Wiki output was not compiled "
				"in. Aborting.\n");
			cli_mfg_abort_usage(argv[0]);
#endif
			break;
		case 'p':
			if (prog != PROGRAMMER_INVALID) {
				msg_gerr("Error: --programmer specified "
					"more than once. You can separate "
					"multiple\nparameters for a programmer "
					"with \",\". Please see the man page "
					"for details.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			for (prog = 0; prog < PROGRAMMER_INVALID; prog++) {
				name = programmer_table[prog].name;
				namelen = strlen(name);
				if (strncmp(optarg, name, namelen) == 0) {
					switch (optarg[namelen]) {
					case ':':
						pparam = strdup(optarg + namelen + 1);
						if (!strlen(pparam)) {
							free(pparam);
							pparam = NULL;
						}
						break;
					case '\0':
						break;
					default:
						/* The continue refers to the
						 * for loop. It is here to be
						 * able to differentiate between
						 * foo and foobar.
						 */
						continue;
					}
					break;
				}
			}

			for (i = 0; aliases[i].name; i++) {
				name = aliases[i].name;
				namelen = strlen(aliases[i].name);

				if (strncmp(optarg, name, namelen))
					continue;

				switch (optarg[namelen]) {
				case ':':
					pparam = strdup(optarg + namelen + 1);
					if (!strlen(pparam)) {
						free(pparam);
						pparam = NULL;
					}
					break;
				case '\0':
					break;
				default:
					/* The continue refers to the for-loop.
					 * It is here to be able to
					 * differentiate between foo and foobar.
					 */
					continue;
				}

				alias = &aliases[i];
				msg_gdbg("Programmer alias: \"%s\", parameter: "
					" \"%s\",\n", alias->name, pparam);

				break;
			}

			if ((prog == PROGRAMMER_INVALID) && !alias) {
				msg_gerr("Error: Unknown programmer "
					"%s.\n", optarg);
				cli_mfg_abort_usage(argv[0]);
			}

			if ((prog != PROGRAMMER_INVALID) && alias) {
				msg_gerr("Error: Alias cannot be used "
					"with programmer name.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			break;
		case 'R':
			/* print_version() is always called during startup. */
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			exit(0);
			break;
		case 'h':
			if (++operation_specified > 1) {
				msg_gerr("More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			cli_mfg_usage(argv[0]);
			exit(0);
			break;
		case LONGOPT_GET_SIZE:
			get_size = 1;
			break;
		case LONGOPT_WP_STATUS:
			wp_status = 1;
			break;
		case LONGOPT_WP_LIST:
			wp_list = 1;
			break;
		case LONGOPT_WP_SET_RANGE:
			set_wp_range = 1;
			break;
		case LONGOPT_WP_ENABLE:
			set_wp_enable = 1;
			if (optarg)
				wp_mode_opt = strdup(optarg);
			break;
		case LONGOPT_WP_DISABLE:
			set_wp_disable = 1;
			break;
		case LONGOPT_FLASH_NAME:
			flash_name = 1;
			break;
		case LONGOPT_DIFF:
			diff_file = strdup(optarg);
			break;
		case LONGOPT_IGNORE_FMAP:
			set_ignore_fmap = 1;
			break;
		case LONGOPT_FAST_VERIFY:
			verify_it = VERIFY_PARTIAL;
			break;
		case 'b':
			broken_timer = 1;
			break;
		case LONGOPT_IGNORE_LOCK:
			set_ignore_lock = 1;
			break;
		default:
			cli_mfg_abort_usage(argv[0]);
			break;
		}
	}

	/* FIXME: Print the actions flashrom will take. */

	if (list_supported) {
		print_supported();
		exit(0);
	}

#if CONFIG_PRINT_WIKI == 1
	if (list_supported_wiki) {
		print_supported_wiki();
		exit(0);
	}
#endif

#if 0
	if (optind < argc) {
		msg_gerr("Error: Extra parameter found.\n");
		cli_mfg_abort_usage(argv[0]);
	}
#endif

	if (read_it || write_it || verify_it) {
		if (argv[optind])
			filename = argv[optind];
	}

	if (chip_to_probe) {
		for (flash = flashchips; flash && flash->name; flash++) {
			if (!strcmp(flash->name, chip_to_probe)) {
				found_chip = 1;
				break;
			}
		}
		for (flash = flashchips_hwseq; flash && flash->name &&
				!found_chip; flash++) {
			if (!strcmp(flash->name, chip_to_probe)) {
				found_chip = 1;
				break;
			}
		}
		if (!found_chip) {
			msg_gerr("Error: Unknown chip '%s' specified.\n",
				chip_to_probe);
			msg_gerr("Run flashrom -L to view the hardware supported "
				"in this flashrom version.\n");
			exit(1);
		}
		/* Clean up after the check. */
		flash = NULL;
	}

	if (prog == PROGRAMMER_INVALID)
		prog = default_programmer;

#if CONFIG_INTERNAL == 1
	if ((prog != PROGRAMMER_INTERNAL) && (lb_part || lb_vendor)) {
		msg_gerr("Error: --mainboard requires the internal "
				"programmer. Aborting.\n");
		cli_mfg_abort_usage(argv[0]);
	}
#endif

#if USE_BIG_LOCK == 1
	/* get lock before doing any work that touches hardware */
	if (!set_ignore_lock) {
		msg_gdbg("Acquiring lock (timeout=%d sec)...\n", LOCK_TIMEOUT_SECS);
		if (acquire_big_lock(LOCK_TIMEOUT_SECS) < 0) {
			msg_gerr("Could not acquire lock.\n");
			exit(1);
		}
		msg_gdbg("Lock acquired.\n");
	}
#endif

	/*
	 * Let powerd know that we're updating firmware so machine stays awake.
	 *
	 * A bit of history behind this small block of code:
	 * chromium-os:15025 - If broken_timer == 1, use busy loop instead of
	 * OS timers to avoid excessive usleep overhead during "long" operations
	 * involving reads, erases, and writes. This was mostly a problem on
	 * old machines with poor DVFS implementations.
	 *
	 * chromium-os:18895 - Disabled power management to prevent system from
	 * going to sleep while doing a destructive operation.
	 *
	 * chromium-os:19321 - Use OS timers for non-destructive operations to
	 * avoid UI jank.
	 *
	 * chromium:400641 - Powerd is smarter now, so instead of stopping it
	 * manually we'll use a file lock so it knows not to put the machine
	 * to sleep or do other things that can interfere.
	 *
	 */
	if (write_it || erase_it)
		disable_power_management();

	/* FIXME: Delay calibration should happen in programmer code. */
	myusec_calibrate_delay();

	/* FIXME: This statement below is arbitrary and only for the case that
	 * internal_init is called from programmer_init. Find a better
	 * solution for this. */
	flashes[0].pgm = &registered_programmers[0];

	if (programmer_init(&flashes[0], prog, pparam)) {
		msg_gerr("Error: Programmer initialization failed.\n");
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	/* FIXME: Delay calibration should happen in programmer code. */
	for (j = 0; j < registered_programmer_count; j++) {
		startchip = 0;
		for (i = 0; i < ARRAY_SIZE(flashes); i++) {
			startchip = probe_flash(&registered_programmers[j],
						startchip, &flashes[i], 0);
			/* FIXME: this if-else code can obviously be simplified, but we are leaving it as
			 * is to retain the condition structure; we may want to look for a better solution
			 * of how to probe, which flash/programmer to select, and when to exit.
			 */
			if (startchip == -1){
				break; /* We may want to continue here instead of breaking (see above) */
			} else {
				chipcount++;
				startchip++;
				break;
			}
		}
		if(chipcount)
			break;
	}

	if (chipcount > 1) {
		msg_gerr("Multiple flash chips were detected:");
		for (i = 0; i < chipcount; i++)
			msg_gerr(" %s", flashes[i].name);
		msg_gerr("\nPlease specify which chip to use with the -c <chipname> option.\n");
		programmer_shutdown(&flashes[0]);
		exit(1);
	} else if (!chipcount) {
		msg_gerr("No EEPROM/flash device found.\n");
		if (!force || !chip_to_probe) {
			msg_gerr("Note: flashrom can never write if the flash chip isn't found automatically.\n");
		}
		if (force && read_it && chip_to_probe) {
			msg_ginfo("Force read (-f -r -c) requested, pretending the chip is there:\n");
			startchip = probe_flash(flashes[0].pgm, 0, &flashes[0], 1);
			if (startchip == -1) {
				msg_gerr("Probing for flash chip '%s' failed.\n", chip_to_probe);
				rc = 1;
				goto cli_mfg_silent_exit;
			}
			msg_ginfo("Please note that forced reads most likely contain garbage.\n");
			return read_flash_to_file(&flashes[0], filename);
		}
		// FIXME: flash writes stay enabled!
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	fill_flash = &flashes[0];
	check_chip_supported(fill_flash);

	size = fill_flash->total_size * 1024;
	if (check_max_decode((fill_flash->pgm->buses_supported & fill_flash->bustype), size) &&
	    (!force)) {
		msg_gerr("Chip is too big for this programmer "
			"(-V gives details). Use --force to override.\n");
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	if (!(read_it | write_it | verify_it | erase_it | flash_name |
	      get_size | set_wp_range | set_wp_enable | set_wp_disable |
	      wp_status | wp_list | extract_it)) {
		msg_gerr("No operations were specified.\n");
		// FIXME: flash writes stay enabled!
		rc = 0;
		goto cli_mfg_silent_exit;
	}

	if (set_wp_enable && set_wp_disable) {
		msg_ginfo("Error: --wp-enable and --wp-disable are mutually exclusive\n");
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	/*
	 * Common rules for -r/-w/-v syntax parsing:
	 * - If no filename is specified at all, quit.
	 * - If no filename is specified for -r/-w/-v, but files are specified
	 *   for -i, then the number of file arguments for -i options must be
	 *   equal to the total number of -i options.
	 *
	 * Rules for reading:
	 * - If files are specified for -i args but not -r, do partial reads for
	 *   each -i arg, creating a new file for each region. Each -i option
	 *   must specify a filename.
	 * - If filenames are specified for -r and -i args, then:
	 *     - Do partial read for each -i arg, creating a new file for
	 *       each region where a filename is provided (-i region:filename).
	 *     - Create a ROM-sized file with partially filled content. For each
	 *       -i arg, fill the corresponding offset with content from ROM.
	 *
	 * Rules for writing and verifying:
	 * - If files are specified for both -w/-v and -i args, -i files take
	 *   priority. (Note: We determined this was the most useful syntax for
	 *   chromium.org's flashrom after some discussion. Upstream may wish
	 *   to quit in this case due to ambiguity).
	 *   See: http://crbug.com/263495.
	 * - If file is specified for -w/-v and no files are specified with -i
	 *   args, then the file is to be used for writing/verifying the entire
	 *   ROM.
	 * - If files are specified for -i args but not -w, do partial writes
	 *   for each -i arg. Likewise for -v and -i args. All -i args must
	 *   supply a filename. Any omission is considered ambiguous.
	 * - Regions with a filename associated must not overlap. This is also
	 *   considered ambiguous. Note: This is checked later since it requires
	 *   processing the layout/fmap first.
	 */
	if (read_it || write_it || verify_it) {
		char op;

		if (read_it)
			op = 'r';
		else if (write_it)
			op = 'w';
		else if (verify_it)
			op = 'v';
		else {
			msg_gerr("Error: Unknown file operation\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		if (!filename) {
			if (!get_num_include_args()) {
				msg_gerr("Error: No file specified for -%c.\n",
						op);
				rc = 1;
				goto cli_mfg_silent_exit;
			}

			if (num_include_files() != get_num_include_args()) {
				msg_gerr("Error: One or more -i arguments is "
					" missing a filename.\n");
				rc = 1;
				goto cli_mfg_silent_exit;
			}
		}
	}

	/* Always verify write operations unless -n is used. */
	if (write_it && !dont_verify_it)
		if (!verify_it) verify_it = VERIFY_FULL;

	/* Partial verify requested, but no -i args: Need to full verify. */
	if (verify_it == VERIFY_PARTIAL && !specified_partition())
		verify_it = VERIFY_FULL;

	/* Note: set_wp_disable should be done before setting the range */
	if (set_wp_disable) {
		if (fill_flash->wp && fill_flash->wp->disable) {
			rc |= fill_flash->wp->disable(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}
	}

	if (flash_name) {
		if (fill_flash->vendor && fill_flash->name) {
			msg_ginfo("vendor=\"%s\" name=\"%s\"\n",
			       fill_flash->vendor, fill_flash->name);
			goto cli_mfg_silent_exit;
		} else {
			rc = -1;
			goto cli_mfg_silent_exit;
		}
	}

	/* Note: set_wp_range must happen before set_wp_enable */
	if (set_wp_range) {
		unsigned int start, len;
		char *endptr = NULL;

		if ((argc - optind) != 2) {
			msg_gerr("Error: invalid number of arguments\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		/* FIXME: add some error checking */
		start = strtoul(argv[optind], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			msg_gerr("Error: value \"%s\" invalid\n", argv[optind]);
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		len = strtoul(argv[optind + 1], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			msg_gerr("Error: value \"%s\" invalid\n", argv[optind + 1]);
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		if (fill_flash->wp && fill_flash->wp->set_range) {
			rc |= fill_flash->wp->set_range(fill_flash, start, len);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}
	}
	
	if (!rc && set_wp_enable) {
		enum wp_mode wp_mode;

		if (wp_mode_opt)
			wp_mode = get_wp_mode(wp_mode_opt);
		else
			wp_mode = WP_MODE_HARDWARE;	/* default */

		if (wp_mode == WP_MODE_UNKNOWN) {
			msg_gerr("Error: Invalid WP mode: \"%s\"\n", wp_mode_opt);
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		if (fill_flash->wp && fill_flash->wp->enable) {
			rc |= fill_flash->wp->enable(fill_flash, wp_mode);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}
	}
	
	if (get_size) {
		msg_ginfo("%d\n", fill_flash->total_size * 1024);
		goto cli_mfg_silent_exit;
	}

	if (wp_status) {
		if (fill_flash->wp && fill_flash->wp->wp_status) {
			rc |= fill_flash->wp->wp_status(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			rc = 1;
		}
		goto cli_mfg_silent_exit;
	}
	
	if (wp_list) {
		msg_ginfo("Valid write protection ranges:\n");
		if (fill_flash->wp && fill_flash->wp->list_ranges) {
			rc |= fill_flash->wp->list_ranges(fill_flash);
		} else {
			msg_gerr("Error: write protect is not supported "
			       "on this flash chip.\n");
			rc = 1;
		}
		goto cli_mfg_silent_exit;
	}

	/* If the user doesn't specify any -i argument, then we can skip the
	 * fmap parsing to speed up. */
	if (get_num_include_args() == 0 && !extract_it) {
		msg_gdbg("No -i argument is specified, set ignore_fmap.\n");
		set_ignore_fmap = 1;
	}

	if (read_it || write_it || erase_it || verify_it || extract_it) {
		rc = doit(fill_flash, force, filename,
		          read_it, write_it, erase_it, verify_it,
		          extract_it, diff_file);
	}

	msg_ginfo("%s\n", rc ? "FAILED" : "SUCCESS");
cli_mfg_silent_exit:
	programmer_shutdown(fill_flash);  /* must be done after chip_restore() */
#if USE_BIG_LOCK == 1
	if (!set_ignore_lock)
		release_big_lock();
#endif
	if (restore_power_management()) {
		msg_gerr("Unable to re-enable power management\n");
		rc |= 1;
	}

	return rc;
}
