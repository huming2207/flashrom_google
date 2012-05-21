/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 The Chromium OS Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "flashchips.h"
#include "fmap.h"
#include "gec_lpc_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

static int ec_timeout_usec = 1000000;

/* Waits for the EC to be unbusy. Returns 1 if busy, 0 if not busy. */
static int ec_busy(int timeout_usec)
{
	int i;
	for (i = 0; i < timeout_usec; i += 10) {
		usleep(10);  /* Delay first, in case we just sent a command */
		if (!(inb(EC_LPC_ADDR_USER_CMD) & EC_LPC_STATUS_BUSY_MASK))
			return 0;
	}
	return 1;  /* Timeout */
}

static enum lpc_status gec_get_result() {
	return inb(EC_LPC_ADDR_USER_DATA);
}

/* Sends a command to the EC.  Returns the command status code, or
 * -1 if other error. */
static int gec_command_lpc(int command, const void *indata, int insize,
			   void *outdata, int outsize) {
	uint8_t *d;
	int i;

	if ((insize + outsize) > EC_LPC_PARAM_SIZE) {
		msg_pdbg2("Data size too big for buffer.\n");
		return -1;
	}

	if (ec_busy(ec_timeout_usec)) {
		msg_pdbg2("Timeout waiting for EC ready\n");
		return -1;
	}

	/* Write data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, d = (uint8_t *)indata; i < insize; i++, d++) {
		msg_pdbg2("GEC: Port[0x%x] <-- 0x%x\n",
		          EC_LPC_ADDR_USER_PARAM + i, *d);
		outb(*d, EC_LPC_ADDR_USER_PARAM + i);
	}

	msg_pdbg2("GEC: Run EC Command: 0x%x ----\n", command);
	outb(command, EC_LPC_ADDR_USER_CMD);

	if (ec_busy(1000000)) {
		msg_pdbg2("Timeout waiting for EC response\n");
		return -1;
	}

	/* Check status */
	if ((i = gec_get_result()) != EC_LPC_RESULT_SUCCESS) {
		msg_pdbg2("EC returned error status %d\n", i);
		return i;
	}

	/* Read data, if any */
	for (i = 0, d = (uint8_t *)outdata; i < outsize; i++, d++) {
		*d = inb(EC_LPC_ADDR_USER_PARAM + i);
		msg_pdbg2("GEC: Port[0x%x] ---> 0x%x\n",
		          EC_LPC_ADDR_USER_PARAM + i, *d);
	}

	return 0;
}

/* Sends HELLO command to ACPI port and expects a value from Google EC.
 *
 * TODO: This is an intrusive command for non-Google ECs. Needs a more proper
 *       and more friendly way to detect.
 */
static int detect_ec(void) {
	struct lpc_params_hello request;
	struct lpc_response_hello response;
	int rc = 0;
	int old_timeout = ec_timeout_usec;

	if (target_bus != BUS_LPC) {
		msg_pdbg("%s():%d target_bus is not LPC.\n", __func__, __LINE__);
		return 1;
	}

	/* reduce timeout period temporarily in case EC is not present */
	ec_timeout_usec = 25000;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	rc = gec_command_lpc(EC_LPC_COMMAND_HELLO, &request,
			     sizeof(request), &response, sizeof(response));

	ec_timeout_usec = old_timeout;

	if (rc || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
#ifdef SUPPORT_CHECKSUM
		/* In this mode, we can tolerate some bit errors. */
		{
			int diff = response.out_data ^ 0xf1e2d3c4;
			if (!(diff = (diff - 1) & diff)) return 0;// 1-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 2-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 3-bit error
			if (!(diff = (diff - 1) & diff)) return 0;// 4-bit error
		}
#endif
		return 1;
	}

	return 0;
}

static struct gec_priv gec_lpc_priv = {
	.detected	= 0,
	.ec_command	= gec_command_lpc,
};

static const struct opaque_programmer opaque_programmer_gec = {
	.max_data_read	= EC_LPC_FLASH_SIZE_MAX,
	.max_data_write	= EC_LPC_FLASH_SIZE_MAX,
	.probe		= gec_probe_size,
	.read		= gec_read,
	.write		= gec_write,
	.erase		= gec_block_erase,
	.data		= &gec_lpc_priv,
};

int gec_probe_lpc(const char *name) {
	msg_pdbg("%s():%d ...\n", __func__, __LINE__);

	if (detect_ec()) return 1;

	if (buses_supported & BUS_SPI) {
		msg_pdbg("%s():%d remove BUS_SPI from buses_supported.\n",
		         __func__, __LINE__);
		buses_supported &= ~BUS_SPI;
	}
	buses_supported |= BUS_LPC;

	msg_pdbg("GEC detected on LPC bus\n");
	register_opaque_programmer(&opaque_programmer_gec);
	gec_lpc_priv.detected = 1;
	return 0;
}
