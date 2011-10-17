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

#define LOCK_TIMEOUT_SECS	30

/* This variable is shared with doit() in flashrom.c */
int set_ignore_fmap = 0;

void cli_mfg_usage(const char *name)
{
	const char *pname;
	int pnamelen;
	int remaining = 0;
	enum programmer p;

	printf("Usage: %s [-n] [-V] [-f] [-h|-R|-L|"
#if CONFIG_PRINT_WIKI == 1
	         "-z|"
#endif
	         "-E|-r <file>|-w <file>|-v <file>] [-i <image>[:<file>]]\n"
	       "       [-c <chipname>] [-m [<vendor>:]<part>] [-l <file>]\n"
	       "       [-p <programmername>[:<parameters>]]\n",
	       name);

	printf("Please note that the command line interface for flashrom has "
	         "changed between\n"
	       "0.9.1 and 0.9.2 and will change again before flashrom 1.0.\n"
	       "Do not use flashrom in scripts or other automated tools "
	         "without checking\n"
	       "that your flashrom version won't interpret options in a "
	         "different way.\n\n");

	printf("   -h | --help                       print this help text\n"
	       "   -R | --version                    print version (release)\n"
	       "   -r | --read <file>                read flash and save to "
	         "<file>\n"
	       "   -w | --write <file>               write <file> to flash\n"
	       "   -v | --verify <file>              verify flash against "
	         "<file>\n"
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
	       "        --fast-verify                only verify -i part\n"
	       "   -l | --layout <file>              read ROM layout from "
	         "<file>\n"
	       "   -i | --image <name>[:<file>]      only access image <name> "
	         "from flash layout.\n"
	       "                                     the content are included "
	         "in <file> if specified.\n"
	       "   -L | --list-supported             print supported devices\n"
#if CONFIG_PRINT_WIKI == 1
	       "   -z | --list-supported-wiki        print supported devices "
	         "in wiki syntax\n"
#endif
	       "   -p | --programmer <name>[:<param>] specify the programmer "
 	         "device\n"
	       "   -b | --broken-timers              assume system timers are "
	         "broken\n"
	       "   --ignore-fmap                     don't try to parse the "
	         "fmap structure on the flash\n"
		 );

	for (p = 0; p < PROGRAMMER_INVALID; p++) {
		pname = programmer_table[p].name;
		pnamelen = strlen(pname);
		if (remaining - pnamelen - 2 < 0) {
			printf("\n                                     ");
			remaining = 43;
		} else {
			printf(" ");
			remaining--;
		}
		if (p == 0) {
			printf("(");
			remaining--;
		}
		printf("%s", pname);
		remaining -= pnamelen;
		if (p < PROGRAMMER_INVALID - 1) {
			printf(",");
			remaining--;
		} else {
			printf(")\n");
		}
	}

	printf("Long-options:\n");
	printf("   --diff <file>                     diff from file instead of ROM\n");
	printf("   --get-size                        get chip size (bytes)\n");
	printf("   --wp-status                       show write protect status\n");
	printf("   --wp-range <start> <length>       set write protect range\n");
	printf("   --wp-enable                       enable write protection\n");
	printf("   --wp-disable                      disable write protection\n");
	printf("   --wp-list                         list write protection ranges\n");

	list_programmers_linebreak(37, 80, 1);
	printf("\nYou can specify one of -h, -R, -L, "
#if CONFIG_PRINT_WIKI == 1
	         "-z, "
#endif
	         "-E, -r, -w, -v or no operation.\n"
	       "If no operation is specified, flashrom will only probe for "
	         "flash chips.\n\n");
}

void cli_mfg_abort_usage(const char *name)
{
	printf("Please run \"%s --help\" for usage info.\n", name);
	exit(1);
}

enum LONGOPT_RETURN_VALUES {
	/* start after ASCII chars */
	LONGOPT_GET_SIZE = 256,
	LONGOPT_DIFF,
	LONGOPT_WP_STATUS,
	LONGOPT_WP_SET_RANGE,
	LONGOPT_WP_ENABLE,
	LONGOPT_WP_DISABLE,
	LONGOPT_WP_LIST,
	LONGOPT_IGNORE_FMAP,
	LONGOPT_FAST_VERIFY,
};

int cli_mfg(int argc, char *argv[])
{
	unsigned long size;
	/* Probe for up to three flash chips. */
	const struct flashchip *flash;
	struct flashchip flashes[3];
	struct flashchip *fill_flash;
	int startchip = 0;
	int chipcount = 0;
	const char *name;
	int namelen;
	int opt;
	int option_index = 0;
	int force = 0;
	int read_it = 0, write_it = 0, erase_it = 0, verify_it = 0,
	    get_size = 0, set_wp_range = 0, set_wp_enable = 0,
	    set_wp_disable = 0, wp_status = 0, wp_list = 0;
	int dont_verify_it = 0, list_supported = 0;
	int diff = 0;
#if CONFIG_PRINT_WIKI == 1
	int list_supported_wiki = 0;
#endif
	int operation_specified = 0;
	int i;
	int rc = 0;

	const char *optstring = "r:Rw:v:nVEfc:m:l:i:p:Lzhb";
	static struct option long_options[] = {
		{"read", 1, 0, 'r'},
		{"write", 1, 0, 'w'},
		{"erase", 0, 0, 'E'},
		{"verify", 1, 0, 'v'},
		{"noverify", 0, 0, 'n'},
		{"chip", 1, 0, 'c'},
		{"mainboard", 1, 0, 'm'},
		{"verbose", 0, 0, 'V'},
		{"force", 0, 0, 'f'},
		{"layout", 1, 0, 'l'},
		{"image", 1, 0, 'i'},
		{"list-supported", 0, 0, 'L'},
		{"list-supported-wiki", 0, 0, 'z'},
		{"programmer", 1, 0, 'p'},
		{"help", 0, 0, 'h'},
		{"version", 0, 0, 'R'},
		{"get-size", 0, 0, LONGOPT_GET_SIZE},
		{"diff", 1, 0, LONGOPT_DIFF},
		{"wp-status", 0, 0, LONGOPT_WP_STATUS},
		{"wp-range", 0, 0, LONGOPT_WP_SET_RANGE},
		{"wp-enable", 0, 0, LONGOPT_WP_ENABLE},
		{"wp-disable", 0, 0, LONGOPT_WP_DISABLE},
		{"wp-list", 0, 0, LONGOPT_WP_LIST},
		{"broken-timers", 0, 0, 'b' },
		{"ignore-fmap", 0, 0, LONGOPT_IGNORE_FMAP},
		{"fast-verify", 0, 0, LONGOPT_FAST_VERIFY},
		{0, 0, 0, 0}
	};

	char *filename = NULL;
	char *diff_file = NULL;

	char *tempstr = NULL;
	char *pparam = NULL;

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
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			filename = strdup(optarg);
			read_it = 1;
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
			break;
		case 'w':
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			filename = strdup(optarg);
			write_it = 1;
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
			break;
		case 'v':
			//FIXME: gracefully handle superfluous -v
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			if (dont_verify_it) {
				fprintf(stderr, "--verify and --noverify are"
					"mutually exclusive. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			filename = strdup(optarg);
			if (!verify_it) verify_it = VERIFY_FULL;
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
			break;
		case 'n':
			if (verify_it) {
				fprintf(stderr, "--verify and --noverify are"
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
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			erase_it = 1;
			/* horrible workaround for excess time spent in
			 * ichspi.c code: */
			broken_timer = 1;
			break;
		case 'm':
#if CONFIG_INTERNAL == 1
			tempstr = strdup(optarg);
			lb_vendor_dev_from_string(tempstr);
#else
			fprintf(stderr, "Error: Internal programmer support "
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
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			list_supported = 1;
			break;
		case 'z':
#if CONFIG_PRINT_WIKI == 1
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			list_supported_wiki = 1;
#else
			fprintf(stderr, "Error: Wiki output was not compiled "
				"in. Aborting.\n");
			cli_mfg_abort_usage(argv[0]);
#endif
			break;
		case 'p':
			for (programmer = 0; programmer < PROGRAMMER_INVALID; programmer++) {
				name = programmer_table[programmer].name;
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
			if (programmer == PROGRAMMER_INVALID) {
				fprintf(stderr, "Error: Unknown programmer "
					"%s.\n", optarg);
				cli_mfg_abort_usage(argv[0]);
			}
			break;
		case 'R':
			/* print_version() is always called during startup. */
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_mfg_abort_usage(argv[0]);
			}
			exit(0);
			break;
		case 'h':
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
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
			break;
		case LONGOPT_WP_DISABLE:
			set_wp_disable = 1;
			break;
		case LONGOPT_DIFF:
			diff = 1;
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
		fprintf(stderr, "Error: Extra parameter found.\n");
		cli_mfg_abort_usage(argv[0]);
	}
#endif

#if CONFIG_INTERNAL == 1
	if ((programmer != PROGRAMMER_INTERNAL) && (lb_part || lb_vendor)) {
		fprintf(stderr, "Error: --mainboard requires the internal "
				"programmer. Aborting.\n");
		cli_mfg_abort_usage(argv[0]);
	}
#endif

	if (chip_to_probe) {
		for (flash = flashchips; flash && flash->name; flash++)
			if (!strcmp(flash->name, chip_to_probe))
				break;
		if (!flash || !flash->name) {
			fprintf(stderr, "Error: Unknown chip '%s' specified.\n",
				chip_to_probe);
			printf("Run flashrom -L to view the hardware supported "
				"in this flashrom version.\n");
			exit(1);
		}
		/* Clean up after the check. */
		flash = NULL;
	}

#if USE_BIG_LOCK == 1
	/* get lock before doing any work that touches hardware */
	msg_gdbg("Acquiring lock (timeout=%d sec)...\n", LOCK_TIMEOUT_SECS);
	if (acquire_big_lock(LOCK_TIMEOUT_SECS) < 0) {
		msg_gerr("Could not acquire lock.\n");
		exit(1);
	}
	msg_gdbg("Lock acquired.\n");
#endif

	/*
	 * Disable power management before timer calibration for potentially-
	 * destructive operations only. Leave power management enabled for
	 * reads to avoid UI jank (see issue chromium-os:19321).
	 */
	if ((write_it || erase_it) && broken_timer)
		disable_power_management();

	/* FIXME: Delay calibration should happen in programmer code. */
	myusec_calibrate_delay();

	if (programmer_init(pparam)) {
		fprintf(stderr, "Error: Programmer initialization failed.\n");
		rc = 1;
		goto cli_mfg_release_lock_exit;
	}

	/* FIXME: Delay calibration should happen in programmer code. */
	for (i = 0; i < ARRAY_SIZE(flashes); i++) {
		startchip = probe_flash(startchip, &flashes[i], 0);
		if (startchip == -1)
			break;
		chipcount++;
		startchip++;
	}

	if (chipcount > 1) {
		printf("Multiple flash chips were detected:");
		for (i = 0; i < chipcount; i++)
			printf(" %s", flashes[i].name);
		printf("\nPlease specify which chip to use with the -c <chipname> option.\n");
		programmer_shutdown();
		exit(1);
	} else if (!chipcount) {
		printf("No EEPROM/flash device found.\n");
		if (!force || !chip_to_probe) {
			printf("Note: flashrom can never write if the flash chip isn't found automatically.\n");
		}
		if (force && read_it && chip_to_probe) {
			printf("Force read (-f -r -c) requested, pretending the chip is there:\n");
			startchip = probe_flash(0, &flashes[0], 1);
			if (startchip == -1) {
				printf("Probing for flash chip '%s' failed.\n", chip_to_probe);
				rc = 1;
				goto cli_mfg_silent_exit;
			}
			printf("Please note that forced reads most likely contain garbage.\n");
			return read_flash_to_file(&flashes[0], filename);
		}
		// FIXME: flash writes stay enabled!
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	fill_flash = &flashes[0];

	check_chip_supported(fill_flash);

	size = fill_flash->total_size * 1024;
	if (check_max_decode((buses_supported & fill_flash->bustype), size) &&
	    (!force)) {
		fprintf(stderr, "Chip is too big for this programmer "
			"(-V gives details). Use --force to override.\n");
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	if (!(read_it | write_it | verify_it | erase_it |
	      get_size | set_wp_range | set_wp_enable | set_wp_disable |
	      wp_status | wp_list)) {
		printf("No operations were specified.\n");
		// FIXME: flash writes stay enabled!
		rc = 0;
		goto cli_mfg_silent_exit;
	}

	if (set_wp_enable && set_wp_disable) {
		printf("Error: --wp-enable and --wp-disable are mutually exclusive\n");
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	if (!filename && (read_it | write_it | verify_it)) {
		printf("Error: No filename specified.\n");
		// FIXME: flash writes stay enabled!
		rc = 1;
		goto cli_mfg_silent_exit;
	}

	/* Always verify write operations unless -n is used. */
	if (write_it && !dont_verify_it)
		if (!verify_it) verify_it = VERIFY_FULL;

	/* Note: set_wp_disable should be done before setting the range */
	if (set_wp_disable) {
		if (fill_flash->wp && fill_flash->wp->disable)
			rc |= fill_flash->wp->disable(fill_flash);
	}

	/* Note: set_wp_range must happen before set_wp_enable */
	if (set_wp_range) {
		unsigned int start, len;
		char *endptr = NULL;

		if ((argc - optind) != 2) {
			printf("Error: invalid number of arguments\n");
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		/* FIXME: add some error checking */
		start = strtoul(argv[optind], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			printf("Error: value \"%s\" invalid\n", argv[optind]);
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		len = strtoul(argv[optind + 1], &endptr, 0);
		if (errno == ERANGE || errno == EINVAL || *endptr != '\0') {
			printf("Error: value \"%s\" invalid\n", argv[optind + 1]);
			rc = 1;
			goto cli_mfg_silent_exit;
		}

		if (fill_flash->wp && fill_flash->wp->set_range)
			rc |= fill_flash->wp->set_range(fill_flash, start, len);
	}
	
	if (!rc && set_wp_enable) {
		if (fill_flash->wp && fill_flash->wp->enable)
			rc |= fill_flash->wp->enable(fill_flash);
	}
	
	if (get_size) {
		printf("%d\n", fill_flash->total_size * 1024);
		goto cli_mfg_silent_exit;
	}

	if (wp_status) {
		if (fill_flash->wp && fill_flash->wp->wp_status)
			rc |= fill_flash->wp->wp_status(fill_flash);
		goto cli_mfg_silent_exit;
	}
	
	if (wp_list) {
		printf("Valid write protection ranges:\n");
		if (fill_flash->wp && fill_flash->wp->list_ranges)
			rc |= fill_flash->wp->list_ranges(fill_flash);
		goto cli_mfg_silent_exit;
	}

	/* If the user doesn't specify any -i argument, then we can skip the
	 * fmap parsing to speed up. */
	if (get_num_include_args() == 0) {
		msg_gdbg("No -i argument is specified, set ignore_fmap.\n");
		set_ignore_fmap = 1;
	}

	if (read_it || write_it || erase_it || verify_it) {
		rc = doit(fill_flash, force, filename,
		          read_it, write_it, erase_it, verify_it,
		          diff_file);
	}

	msg_ginfo("%s\n", rc ? "FAILED" : "SUCCESS");
cli_mfg_silent_exit:
	programmer_shutdown();  /* must be done after chip_restore() */
cli_mfg_release_lock_exit:
#if USE_BIG_LOCK == 1
	release_big_lock();
#endif
	if (restore_power_management()) {
		msg_perr("Unable to re-enable power management\n");
		rc |= 1;
	}

	return rc;
}
