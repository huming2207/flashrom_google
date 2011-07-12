/*
 * dump information and binaries from BIOS images that are in descriptor mode/soft-strap
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * authors:
 * (c) 2010  Matthias Wenzel <bios at mazzoo dot de>
 * (c) 2011  Stefan Tauner
 *
 * */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "flash.h"
#include "ich_descriptors.h"

extern struct flash_descriptor fdbar;
extern struct flash_component fcba;
extern struct flash_region frba;
extern struct flash_master fmba;
extern struct flash_strap fisba;
extern struct flash_upper_map flumap;
extern struct flash_descriptor_addresses desc_addr;

void dump_file_descriptor(char *fn, uint32_t *fm)
{
	char * n = malloc(strlen(fn) + 11);
	snprintf(n, strlen(fn) + 11, "%s.descr.bin", fn);
	printf("\n");
	printf("+++ dumping %s ... ", n);
	int fh = open(n, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	free(n);
	if (fh < 0)
	{
		printf("ERROR: couldn't open(%s): %s\n", n, strerror(errno));
		exit(1);
	}

	int ret;
	ret = write(fh, &fm[frba.reg0_base >> 2], frba.reg0_limit);
	if (ret != frba.reg0_limit)
	{
		printf("FAILED.\n");
		exit(1);
	}

	printf("done.\n");

	close(fh);
}

void dump_file_BIOS(char *fn, uint32_t *fm)
{
	char * n = malloc(strlen(fn) + 10);
	snprintf(n, strlen(fn) + 10, "%s.BIOS.bin", fn);
	printf("\n");
	printf("+++ dumping %s ... ", n);
	int fh = open(n, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	free(n);
	if (fh < 0)
	{
		printf("ERROR: couldn't open(%s): %s\n", n, strerror(errno));
		exit(1);
	}

	int ret;
	ret = write(fh, &fm[frba.reg1_base >> 2], frba.reg1_limit);
	if (ret != frba.reg1_limit)
	{
		printf("FAILED.\n");
		exit(1);
	}

	printf("done.\n");

	close(fh);
}

void dump_file_ME(char *fn, uint32_t *fm)
{
	char * n = malloc(strlen(fn) + 8);
	snprintf(n, strlen(fn) + 8, "%s.ME.bin", fn);
	printf("\n");
	printf("+++ dumping %s ... ", n);
	int fh = open(n, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	free(n);
	if (fh < 0)
	{
		printf("ERROR: couldn't open(%s): %s\n", n, strerror(errno));
		exit(1);
	}

	int ret;
	ret = write(fh, &fm[frba.reg2_base >> 2], frba.reg2_limit);
	if (ret != frba.reg2_limit)
	{
		printf("FAILED.\n");
		exit(1);
	}

	printf("done.\n");

	close(fh);
}

void dump_file_GbE(char *fn, uint32_t *fm)
{
	char * n = malloc(strlen(fn) + 9);
	snprintf(n, strlen(fn) + 9, "%s.GbE.bin", fn);
	printf("\n");
	printf("+++ dumping %s ... ", n);
	int fh = open(n, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	free(n);
	if (fh < 0)
	{
		printf("ERROR: couldn't open(%s): %s\n", n, strerror(errno));
		exit(1);
	}

	int ret;
	ret = write(fh, &fm[frba.reg3_base >> 2], frba.reg3_limit);
	if (ret != frba.reg3_limit)
	{
		printf("FAILED.\n");
		exit(1);
	}

	printf("done.\n");
	uint8_t * pMAC = (uint8_t *) &fm[frba.reg3_base >> 2];
	printf("the MAC-address might be: %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
		pMAC[0],
		pMAC[1],
		pMAC[2],
		pMAC[3],
		pMAC[4],
		pMAC[5]
		);

	close(fh);
}

void dump_files(char *n, uint32_t *fm)
{
	printf("=== dumping section files ===\n");
	if (frba.reg0_limit)
		dump_file_descriptor(n, fm);
	if (frba.reg1_limit)
		dump_file_BIOS(n, fm);
	if (frba.reg2_limit)
		dump_file_ME(n, fm);
	if (frba.reg3_limit)
		dump_file_GbE(n, fm);
}

void usage(char *argv[], char *error)
{
	if (error != NULL) {
		printf("%s\n", error);
		printf("\n");
	}
	printf("usage: '%s -f <image file name> [-c <chipset name>] [-d]'\n\n"
"where <image file name> points to an image of the contents of the SPI flash.\n"
"In case that image is really in descriptor mode %s\n"
"will pretty print some of the contained information.\n"
"To also print the data stored in the descriptor strap you have to indicate\n"
"the chipset series with the '-c' parameter and one of the possible arguments:\n"
"\t- \"ich8\",\n"
"\t- \"ich9\",\n"
"\t- \"ich10\",\n"
"\t- \"5\" or \"ibex\" for Intel's 5 series chipsets,\n"
"\t- \"6\" or \"cougar\" for Intel's 6 series chipsets,\n"
"\t- \"7\" or \"panther\" for Intel's 7 series chipsets.\n"
"If '-d' is specified some sections such as the BIOS image as seen by the CPU or\n"
"the GbE blob that is required to initialize the GbE are also dumped to files.\n",
	argv[0], argv[0]);
	exit(1);
}

int main(int argc, char *argv[])
{
	int f;			/* file descriptor to flash file */
	unsigned int fs;	/* file size */
	uint32_t *fm;		/* mmap'd file */

	int opt;
	int dump = 0;
	const char *fn = NULL;
	const char *csn = NULL;
	enum chipset cs = CHIPSET_UNKNOWN;

	while ((opt = getopt(argc, argv, "df:c:")) != -1) {
		switch (opt) {
		case 'd':
			dump = 1;
			break;
		case 'f':
			fn = optarg;
			break;
		case 'c':
			csn = optarg;
			break;
		default: /* '?' */
			usage(argv, NULL);
		}
	}
	if (fn == NULL)
		usage(argv,
		      "Need a file name of a descriptor image to read from.");

	f = open(fn, O_RDONLY);
	if (f < 0)
		usage(argv, "No such file");
	fs = lseek(f, 0, SEEK_END);
	if (fs < 0)
		usage(argv, "Seeking to the end of the file failed");

	fm = mmap(NULL, fs, PROT_READ, MAP_PRIVATE, f, 0);
	if (fm == (void *) -1) {
		/* fallback for stupid OSes like cygwin */
		int ret;
		fm = malloc(fs);
		if (!fm)
			usage(argv, "Could not allocate memory");
		lseek(f, 0, SEEK_SET);
		ret = read(f, fm, fs);
		if (ret != fs)
			usage(argv, "Seeking to the end of the file failed");
	}
	printf("flash image has a size of %d [0x%x] bytes.\n", fs, fs);
	close(f);

	if (csn != NULL) {
		if (strcmp(csn, "ich8") == 0)
			cs = CHIPSET_ICH8;
		else if (strcmp(csn, "ich9") == 0)
			cs = CHIPSET_ICH9;
		else if (strcmp(csn, "ich10") == 0)
			cs = CHIPSET_ICH10;
		else if ((strcmp(csn, "5") == 0) ||
			 (strcmp(csn, "ibex") == 0))
			cs = CHIPSET_SERIES_5_IBEX_PEAK;
		else if ((strcmp(csn, "6") == 0) ||
			 (strcmp(csn, "cougar") == 0))
			cs = CHIPSET_SERIES_6_COUGAR_POINT;
		else if ((strcmp(csn, "7") == 0) ||
			 (strcmp(csn, "panther") == 0))
			cs = CHIPSET_SERIES_7_PANTHER_POINT;
	}

	if(read_ich_descriptors_from_dump(fm, cs)){
		printf("not in descriptor mode\n");
		exit(1);
	}

	prettyprint_ich_descriptors(cs);

	if (dump == 1)
		dump_files(argv[1], fm);

	return 0;
}

