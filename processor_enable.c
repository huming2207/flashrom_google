/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2010 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
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

/*
 * Contains the processor specific flash enables and system settings.
 */

#include "flash.h"
#include "programmer.h"

#if defined(__i386__) || defined(__x86_64__)

int processor_flash_enable(void)
{
	/* On x86, flash access is not processor specific except on
	 * AMD Elan SC520, AMD Geode and maybe other SoC-style CPUs.
	 * FIXME: Move enable_flash_cs5536 and get_flashbase_sc520 here.
	 */
	return 0;
}

#else

#if defined (__MIPSEL__) && defined (__linux)
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int is_loongson(void)
{
	FILE *cpuinfo;
	cpuinfo = fopen("/proc/cpuinfo", "rb");
	if (!cpuinfo)
		return 0;
	while (!feof(cpuinfo)) {
		char line[512], *ptr;
		if (fgets(line, sizeof(line), cpuinfo) == NULL)
			break;
		ptr = line;
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		/* "cpu" part appears only with some Linux versions.  */
		if (strncmp(ptr, "cpu", strlen("cpu")) == 0)
			ptr += strlen("cpu");
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		if (strncmp(ptr, "model", strlen("model")) != 0)
			continue;
		ptr += strlen("model");
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		if (*ptr != ':')
			continue;
		ptr++;
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		fclose(cpuinfo);
		return (strncmp(ptr, "ICT Loongson-2 V0.3",
				strlen("ICT Loongson-2 V0.3")) == 0)
		    || (strncmp(ptr, "Godson2 V0.3  FPU V0.1",
				strlen("Godson2 V0.3  FPU V0.1")) == 0);
	}
	fclose(cpuinfo);
	return 0;
}
#endif

#if defined(__arm__)
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Returns true if the /proc/cpuinfo indicates we're a tegra2.
 *
 * This means that it contains the two lines:
 *   CPU part *: *0xc09
 *   CPU variant *: *0x1
 *
 * The "variant" is important because we don't yet support Tegra3 and Tegra3
 * identifies itself as variant 0x2.
 *
 * TODO: need to extend in future for same SPI controller in chip family.
 */
static int is_tegra2(void)
{
	FILE *cpuinfo;
	uint32_t impl = 0, architecture = 0, variant = 0, part = 0;
	const char *part_name = "CPU part";
	const char *part_value = "0xc09";
	const char *variant_name = "CPU variant";
	const char *variant_value = "0x1";
	int found_part = 0;
	int found_variant = 0;
	const char *cur_value = NULL;
	int *cur_found = NULL;

	cpuinfo = fopen("/proc/cpuinfo", "rb");
	if (!cpuinfo)
		return 0;
	while (!feof(cpuinfo)) {
		char line[512], *ptr;
		if (fgets(line, sizeof(line), cpuinfo) == NULL)
			break;
		ptr = line;
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		if (strncmp(ptr, variant_name, strlen(variant_name)) == 0) {
			ptr += strlen(variant_name);
			cur_value = variant_value;
			cur_found = &found_variant;
		} else if (strncmp(ptr, part_name, strlen(part_name)) == 0) {
			ptr += strlen(part_name);
			cur_value = part_value;
			cur_found = &found_part;
		}
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		if (*ptr != ':')
			continue;
		ptr++;
		while (*ptr && isspace((unsigned char)*ptr))
			ptr++;
		if (cur_found)
			*cur_found = (strncmp(ptr, cur_value,
					      strlen(cur_value)) == 0);
		cur_found = NULL;
	}
	fclose(cpuinfo);
	return found_part && found_variant;
}
#endif

int processor_flash_enable(void)
{
	/* FIXME: detect loongson on FreeBSD and OpenBSD as well.  */
#if defined (__MIPSEL__) && defined (__linux)
	if (is_loongson()) {
		flashbase = 0x1fc00000;
		return 0;
	}
#endif
#if defined (__arm__)
	/* FIXME(yjlou): 0xc09 seems ARMv7, not tegra* specified.
	 *               Also, the tegra2_spi_init() should move out of
	 *               the processor enable code logic. */
	if (is_tegra2()) {
		msg_pdbg("Detected NVIDIA Tegra 2.\n");
		return tegra2_spi_init();
	}
#endif
	/* Not implemented yet. Oh well. */
	return 1;
}

#endif
