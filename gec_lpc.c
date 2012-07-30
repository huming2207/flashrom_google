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
#include <sys/param.h>
#include <unistd.h>
#include "flashchips.h"
#include "fmap.h"
#include "gec_ec_commands.h"
#include "programmer.h"
#include "spi.h"
#include "writeprotect.h"

#define INITIAL_UDELAY 10     /* 10 us */
#define MAXIMUM_UDELAY 10000  /* 10 ms */
static int ec_timeout_usec = 1000000;
static int lpc_cmd_args_supported;


/*
 * Wait for the EC to be unbusy.  Returns 0 if unbusy, non-zero if
 * timeout.
 */
static int wait_for_ec(int status_addr, int timeout_usec)
{
	int i;
	int delay = INITIAL_UDELAY;

	for (i = 0; i < timeout_usec; i += delay) {
		/*
		 * Delay first, in case we just sent out a command but the EC
		 * hasn't raise the busy flag. However, I think this doesn't
		 * happen since the LPC commands are executed in order and the
		 * busy flag is set by hardware.
		 *
		 * TODO: move this delay after inb(status).
		 */
		usleep(MIN(delay, timeout_usec - i));

		if (!(inb(status_addr) & EC_LPC_STATUS_BUSY_MASK))
			return 0;

		/* Increase the delay interval after a few rapid checks */
		if (i > 20)
			delay = MIN(delay * 2, MAXIMUM_UDELAY);
	}
	return -1;  /* Timeout */
}


/*
 **************************** EC API v0 ****************************
 */
static enum ec_status gec_get_result() {
	return inb(EC_LPC_ADDR_HOST_DATA);
}

/* Sends a command to the EC.  Returns the command status code, or
 * -1 if other error. */
static int gec_command_lpc_old(int command, const void *outdata, int outsize,
			       void *indata, int insize) {
	uint8_t *d;
	int i;

	if ((outsize > EC_OLD_PARAM_SIZE) ||
	    (insize > EC_OLD_PARAM_SIZE)) {
		msg_pdbg2("Data size too big for buffer.\n");
		return -EC_RES_INVALID_PARAM;
	}

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_pdbg2("Timeout waiting for EC ready\n");
		return -EC_RES_ERROR;
	}

	/* Write data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, d = (uint8_t *)outdata; i < outsize; i++, d++) {
		msg_pdbg2("GEC: Port[0x%x] <-- 0x%x\n",
		          EC_LPC_ADDR_OLD_PARAM + i, *d);
		outb(*d, EC_LPC_ADDR_OLD_PARAM + i);
	}

	msg_pdbg2("GEC: Run EC Command: 0x%x ----\n", command);
	outb(command, EC_LPC_ADDR_HOST_CMD);

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_pdbg2("Timeout waiting for EC response\n");
		return -EC_RES_ERROR;
	}

	/* Check status */
	if ((i = gec_get_result()) != EC_RES_SUCCESS) {
		msg_pdbg2("EC returned error status %d\n", i);
		return -i;
	}

	/* Read data, if any */
	for (i = 0, d = (uint8_t *)indata; i < insize; i++, d++) {
		*d = inb(EC_LPC_ADDR_OLD_PARAM + i);
		msg_pdbg2("GEC: Port[0x%x] ---> 0x%x\n",
		          EC_LPC_ADDR_OLD_PARAM + i, *d);
	}

	return 0;
}


/*
 **************************** EC API v1 ****************************
 */
static int gec_command_lpc(int command, int version,
	                   const void *outdata, int outsize,
	                   void *indata, int insize) {

	struct ec_lpc_host_args args;
	const uint8_t *dout;
	uint8_t *din;
	int csum;
	int i;

	/* Fall back to old-style command interface if args aren't supported */
	if (!lpc_cmd_args_supported)
		return gec_command_lpc_old(command, outdata, outsize,
				           indata, insize);

	/* Fill in args */
	args.flags = EC_HOST_ARGS_FLAG_FROM_HOST;
	args.command_version = version;
	args.data_size = outsize;

	/* Calculate checksum */
	csum = command + args.flags + args.command_version + args.data_size;
	for (i = 0, dout = (const uint8_t *)outdata; i < outsize; i++, dout++)
		csum += *dout;

	args.checksum = (uint8_t)csum;

	/* Write args */
	for (i = 0, dout = (const uint8_t *)&args;
	     i < sizeof(args);
	     i++, dout++)
		outb(*dout, EC_LPC_ADDR_HOST_ARGS + i);

	/* Write data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, dout = (uint8_t *)outdata; i < outsize; i++, dout++)
		outb(*dout, EC_LPC_ADDR_HOST_PARAM + i);

	outb(command, EC_LPC_ADDR_HOST_CMD);

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_perr("Timeout waiting for EC response\n");
		return -EC_RES_ERROR;
	}

	/* Check result */
	i = inb(EC_LPC_ADDR_HOST_DATA);
	if (i) {
		msg_pdbg2("EC returned error result code %d\n", i);
		return -i;
	}

	/* Read back args */
	for (i = 0, din = (uint8_t *)&args; i < sizeof(args); i++, din++)
		*din = inb(EC_LPC_ADDR_HOST_ARGS + i);

	/*
	 * If EC didn't modify args flags, then somehow we sent a new-style
	 * command to an old EC, which means it would have read its params
	 * from the wrong place.
	 */
	if (!(args.flags & EC_HOST_ARGS_FLAG_TO_HOST)) {
		msg_perr("EC protocol mismatch\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (args.data_size > insize) {
		msg_perr("EC returned too much data\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	/* Read data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, din = (uint8_t *)indata; i < args.data_size; i++, din++)
		*din = inb(EC_LPC_ADDR_HOST_PARAM + i);

	/* Verify checksum */
	csum = command + args.flags + args.command_version + args.data_size;
	for (i = 0, din = (uint8_t *)indata; i < args.data_size; i++, din++)
		csum += *din;

	if (args.checksum != (uint8_t)csum) {
		msg_perr("EC response has invalid checksum\n");
		return -EC_RES_INVALID_CHECKSUM;
	}

	/* Return actual amount of data received */
	return args.data_size;
}


/*
 * The algorithm is following:
 *
 *   1. If you detect EC command args support, success.
 *   2. If all ports read 0xff, fail.
 *   3. Try hello command (for API v0).
 *
 * TODO: This is an intrusive command for non-Google ECs. Needs a more proper
 *       and more friendly way to detect.
 */
static int detect_ec(void) {
	int i;
	int byte = 0xff;
	struct ec_params_hello request;
	struct ec_response_hello response;
	int rc = 0;
	int old_timeout = ec_timeout_usec;

	if (target_bus != BUS_LPC) {
		msg_pdbg("%s():%d target_bus is not LPC.\n", __func__, __LINE__);
		return 1;
	}

	/*
	 * Test if LPC command args are supported.
	 *
	 * The cheapest way to do this is by looking for the memory-mapped
	 * flag.  This is faster than sending a new-style 'hello' command and
	 * seeing whether the EC sets the EC_HOST_ARGS_FLAG_FROM_HOST flag
	 * in args when it responds.
	 */
	if (inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID) == 'E' &&
	    inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID + 1) == 'C' &&
	    (inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_HOST_CMD_FLAGS) &
	     EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED)) {
		msg_pdbg("%s(): new EC API is detected.\n", __func__);
		lpc_cmd_args_supported = 1;
		return 0;
	} else {
		msg_pdbg("%s(): use legacy EC API.\n", __func__);
	}

	/*
	 * Test if the I/O port has been configured for Chromium EC LPC
	 * interface.  If all the bytes are 0xff, very likely that Chromium EC
	 * is not present.
	 *
	 * TODO: (crosbug.com/p/10963) Should only need to look at the command
	 * byte, since we don't support ACPI burst mode and thus bit 4 should
	 * be 0.
	 */
	byte &= inb(EC_LPC_ADDR_HOST_CMD);
	byte &= inb(EC_LPC_ADDR_HOST_DATA);
	for (i = 0; i < EC_OLD_PARAM_SIZE && byte == 0xff; ++i)
		byte &= inb(EC_LPC_ADDR_OLD_PARAM + i);
	if (byte == 0xff) {
		msg_pdbg("Port 0x%x,0x%x,0x%x-0x%x are all 0xFF.\n",
			 EC_LPC_ADDR_HOST_CMD, EC_LPC_ADDR_HOST_DATA,
			 EC_LPC_ADDR_OLD_PARAM,
			 EC_LPC_ADDR_OLD_PARAM + EC_OLD_PARAM_SIZE - 1);
		msg_pdbg(
			"Very likely this board doesn't have a Chromium EC.\n");
		return 1;
	}

	/* Try hello command -- for EC only supports API v0
	 * TODO: (crosbug.com/p/33102) Remove after MP.
	 */
	/* reduce timeout period temporarily in case EC is not present */
	ec_timeout_usec = 25000;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	rc = gec_command_lpc_old(EC_CMD_HELLO, &request,
				 sizeof(request), &response, sizeof(response));

	ec_timeout_usec = old_timeout;

	if (rc || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("HELLO response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
		return 1;
	}

	return 0;
}


static struct gec_priv gec_lpc_priv = {
	.detected	= 0,
	.ec_command	= gec_command_lpc,
};

static const struct opaque_programmer opaque_programmer_gec = {
	.max_data_read	= EC_OLD_PARAM_SIZE,
	.max_data_write	= 64,
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
