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
#if USE_CROS_EC_LOCK == 1
#include "cros_ec_lock.h"
#endif
#include "cros_ec_commands.h"
#include "programmer.h"
#include "cros_ec.h"
#include "spi.h"
#include "writeprotect.h"

#define INITIAL_UDELAY 10     /* 10 us */
#define MAXIMUM_UDELAY 10000  /* 10 ms */
static int ec_timeout_usec = 1000000;

#define CROS_EC_LOCK_TIMEOUT_SECS 30  /* 30 secs */


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

/* preserve legacy naming to be consistent with legacy implementation. */
#define EC_LPC_ADDR_OLD_PARAM	EC_HOST_CMD_REGION1
#define EC_OLD_PARAM_SIZE	EC_HOST_CMD_REGION_SIZE

static enum ec_status cros_ec_get_result() {
	return inb(EC_LPC_ADDR_HOST_DATA);
}

/* Sends a command to the EC.  Returns the command status code, or
 * -1 if other error. */
static int cros_ec_command_lpc_old(int command, int version,
	                   const void *outdata, int outsize,
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
		msg_pdbg2("CROS_EC: Port[0x%x] <-- 0x%x\n",
		          EC_LPC_ADDR_OLD_PARAM + i, *d);
		outb(*d, EC_LPC_ADDR_OLD_PARAM + i);
	}

	msg_pdbg2("CROS_EC: Run EC Command: 0x%x ----\n", command);
	outb(command, EC_LPC_ADDR_HOST_CMD);

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_pdbg2("Timeout waiting for EC response\n");
		return -EC_RES_ERROR;
	}

	/* Check status */
	if ((i = cros_ec_get_result()) != EC_RES_SUCCESS) {
		msg_pdbg2("EC returned error status %d\n", i);
		return -i;
	}

	/* Read data, if any */
	for (i = 0, d = (uint8_t *)indata; i < insize; i++, d++) {
		*d = inb(EC_LPC_ADDR_OLD_PARAM + i);
		msg_pdbg2("CROS_EC: Port[0x%x] ---> 0x%x\n",
		          EC_LPC_ADDR_OLD_PARAM + i, *d);
	}

	return 0;
}

/*
 **************************** EC API v2 ****************************
 */
static int cros_ec_command_lpc(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize,
		int (*read_memmap)(uint16_t, uint8_t *),
		int (*write_memmap)(uint8_t, uint16_t)) {

	struct ec_lpc_host_args args;
	const uint8_t *dout;
	uint8_t *din;
	int csum;
	int i;

	/* Fill in args */
	args.flags = EC_HOST_ARGS_FLAG_FROM_HOST;
	args.command_version = version;
	args.data_size = outsize;

	/* Calculate checksum */
	csum = command + args.flags + args.command_version + args.data_size;
	for (i = 0, dout = (const uint8_t *)outdata; i < outsize; i++, dout++)
		csum += *dout;

	args.checksum = (uint8_t)csum;

	/* Wait EC is ready to accept command before writing anything to
	 * parameter.
	 */
	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_pdbg("Timeout waiting for EC ready\n");
		return -EC_RES_ERROR;
	}

	/* Write args */
	for (i = 0, dout = (const uint8_t *)&args;
	     i < sizeof(args);
	     i++, dout++)
		if (write_memmap(*dout, EC_LPC_ADDR_HOST_ARGS + i))
			return -EC_RES_ERROR;

	/* Write data, if any */
	/* TODO: optimized copy using outl() */
	for (i = 0, dout = (uint8_t *)outdata; i < outsize; i++, dout++)
		if (write_memmap(*dout, EC_LPC_ADDR_HOST_PARAM + i))
			return -EC_RES_ERROR;

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
		if (read_memmap(EC_LPC_ADDR_HOST_ARGS + i, din))
			return -EC_RES_ERROR;

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
		if (read_memmap(EC_LPC_ADDR_HOST_PARAM + i, din))
			return -EC_RES_ERROR;

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
 **************************** EC API v3 ****************************
 */
static int cros_ec_command_lpc_v3(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize,
		int (*read_memmap)(uint16_t, uint8_t *),
		int (*write_memmap)(uint8_t, uint16_t)) {

	struct ec_host_request rq;
	struct ec_host_response rs;
	const uint8_t *d;
	uint8_t *dout;
	int csum = 0;
	int i;

	/* Fail if output size is too big */
	if (outsize + sizeof(rq) > EC_LPC_HOST_PACKET_SIZE)
		return -EC_RES_REQUEST_TRUNCATED;

	/* Fill in request packet */
	/* TODO(crosbug.com/p/23825): This should be common to all protocols */
	rq.struct_version = EC_HOST_REQUEST_VERSION;
	rq.checksum = 0;
	rq.command = command;
	rq.command_version = version;
	rq.reserved = 0;
	rq.data_len = outsize;

	/* Copy data and start checksum */
	for (i = 0, d = (const uint8_t *)outdata; i < outsize; i++, d++) {
		if (write_memmap(*d, EC_LPC_ADDR_HOST_PACKET + sizeof(rq) + i))
			return -EC_RES_ERROR;
		csum += *d;
	}

	/* Finish checksum */
	for (i = 0, d = (const uint8_t *)&rq; i < sizeof(rq); i++, d++)
		csum += *d;

	/* Write checksum field so the entire packet sums to 0 */
	rq.checksum = (uint8_t)(-csum);

	/* Copy header */
	for (i = 0, d = (const uint8_t *)&rq; i < sizeof(rq); i++, d++)
		if (write_memmap(*d, EC_LPC_ADDR_HOST_PACKET + i))
			return -EC_RES_ERROR;

	/* Start the command */
	outb(EC_COMMAND_PROTOCOL_3, EC_LPC_ADDR_HOST_CMD);

	if (wait_for_ec(EC_LPC_ADDR_HOST_CMD, ec_timeout_usec)) {
		msg_perr("Timeout waiting for EC response\n");
		return -EC_RES_ERROR;
	}

	/* Check result */
	i = inb(EC_LPC_ADDR_HOST_DATA);
	if (i) {
		msg_perr("EC returned error result code %d\n", i);
		return -i;
	}

	/* Read back response header and start checksum */
	csum = 0;
	for (i = 0, dout = (uint8_t *)&rs; i < sizeof(rs); i++, dout++) {
		if (read_memmap(EC_LPC_ADDR_HOST_PACKET + i, dout))
			return -EC_RES_ERROR;
		csum += *dout;
	}

	if (rs.struct_version != EC_HOST_RESPONSE_VERSION) {
		msg_perr("EC response version mismatch\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (rs.reserved) {
		msg_perr("EC response reserved != 0\n");
		return -EC_RES_INVALID_RESPONSE;
	}

	if (rs.data_len > insize) {
		msg_perr("EC returned too much data\n");
		return -EC_RES_RESPONSE_TOO_BIG;
	}

	/* Read back data and update checksum */
	for (i = 0, dout = (uint8_t *)indata; i < rs.data_len; i++, dout++) {
		if (read_memmap(EC_LPC_ADDR_HOST_PACKET + sizeof(rs) + i, dout))
			return -EC_RES_ERROR;
		csum += *dout;
	}

	/* Verify checksum */
	if ((uint8_t)csum) {
		msg_perr("EC response has invalid checksum\n");
		return -EC_RES_INVALID_CHECKSUM;
	}

	/* Return actual amount of data received */
	return rs.data_len;
}

static int read_memmap_lm4(uint16_t port, uint8_t * value)
{
	*value = inb(port);
	return 0;
}

static int write_memmap_lm4(uint8_t value, uint16_t port)
{
	outb(value, port);
	return 0;
}

/*
 ******************** MEC1322 Support Function *********************
 */
extern int mec1322_read_memmap(uint16_t addr, uint8_t *b);
extern int mec1322_write_memmap(uint8_t b, uint16_t addr);

const struct {
        uint8_t (*inb)(uint16_t);
        uint32_t (*inl)(uint16_t);
        void (*outl)(uint32_t, uint16_t);
        int (*usleep)(unsigned int);
} mec1322_host_func = {
        .inb = inb,
        .inl = inl,
        .outl = outl,
        .usleep = usleep,
};

/*
 **************************** EC API v2 ****************************
 */
static int cros_ec_command_lpc_lm4(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize) {
	return cros_ec_command_lpc(command, version, outdata, outsize,
		indata, insize, read_memmap_lm4, write_memmap_lm4);
}

static int cros_ec_command_lpc_mec1322(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize) {
	return cros_ec_command_lpc(command, version, outdata, outsize,
		indata, insize, mec1322_read_memmap, mec1322_write_memmap);
}

/*
 **************************** EC API v3 ****************************
 */
static int cros_ec_command_lpc_v3_lm4(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize) {
	return cros_ec_command_lpc_v3(command, version, outdata, outsize,
		indata, insize, read_memmap_lm4, write_memmap_lm4);
}

static int cros_ec_command_lpc_v3_mec1322(int command, int version,
		const void *outdata, int outsize,
		void *indata, int insize) {
	return cros_ec_command_lpc_v3(command, version, outdata, outsize,
		indata, insize, mec1322_read_memmap, mec1322_write_memmap);
}

static struct cros_ec_priv cros_ec_lpc_priv = {
	.detected	= 0,
	.ec_command	= cros_ec_command_lpc_lm4,
};

static struct opaque_programmer cros_ec = {
	.max_data_read	= EC_HOST_CMD_REGION_SIZE,
	.max_data_write	= 64,
	.probe		= cros_ec_probe_size,
	.read		= cros_ec_read,
	.write		= cros_ec_write,
	.erase		= cros_ec_block_erase,
	.data		= &cros_ec_lpc_priv,
};

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
static int detect_ec(struct cros_ec_priv *priv) {
	int i;
	int byte = 0xff;
	int old_timeout = ec_timeout_usec;
	uint8_t x, y;

#if USE_CROS_EC_LOCK == 1
	msg_gdbg("Acquiring CROS_EC lock (timeout=%d sec)...\n",
		  CROS_EC_LOCK_TIMEOUT_SECS);
	if (acquire_cros_ec_lock(CROS_EC_LOCK_TIMEOUT_SECS) < 0) {
		msg_gerr("Could not acquire CROS_EC lock.\n");
		return 1;
	}
#endif

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

	/*
	 * Test if LPC command args are supported.
	 *
	 * The cheapest way to do this is by looking for the memory-mapped
	 * flag.  This is faster than sending a new-style 'hello' command and
	 * seeing whether the EC sets the EC_HOST_ARGS_FLAG_FROM_HOST flag
	 * in args when it responds.
	 */
	if (inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID) != 'E' ||
	    inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID + 1) != 'C') {
		/* Probe MEC1322 */
		mec1322_read_memmap(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID, &x);
		mec1322_read_memmap(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_ID + 1, &y);
		if ((x != 'E') || (y != 'C')) {
			msg_perr("Missing Chromium EC memory map.\n");
			return -1;
		} else {
			mec1322_read_memmap(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_HOST_CMD_FLAGS, &x);
			if (x & EC_HOST_CMD_FLAG_VERSION_3) {
				/* Protocol version 3 */
				priv->ec_command = cros_ec_command_lpc_v3_mec1322;
				cros_ec.max_data_write = 248 - sizeof(struct ec_host_request);
				cros_ec.max_data_read = 248 - sizeof(struct ec_host_response);
				msg_pdbg("MEC1322 protocol V3 detected.\n");
			} else if (x & EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED) {
				/* Protocol version 2 */
				priv->ec_command = cros_ec_command_lpc_mec1322;
				msg_pdbg("MEC1322 protocol V2 detected.\n");
			} else {
				msg_perr("Bad protocol version on MEC1322.\n");
				return -1;
			}
		}
	} else {
		i = inb(EC_LPC_ADDR_MEMMAP + EC_MEMMAP_HOST_CMD_FLAGS);

		if (i & EC_HOST_CMD_FLAG_VERSION_3) {
			/* Protocol version 3 */
			priv->ec_command = cros_ec_command_lpc_v3_lm4;
#if 0
			/* FIXME(dhendrix): Overflow errors occurred when using
			   EC_LPC_HOST_PACKET_SIZE */
			cros_ec.max_data_write =
				EC_LPC_HOST_PACKET_SIZE - sizeof(struct ec_host_request);
			cros_ec.max_data_read =
				EC_LPC_HOST_PACKET_SIZE - sizeof(struct ec_host_response);
#endif
			cros_ec.max_data_write = 128 - sizeof(struct ec_host_request);
			cros_ec.max_data_read = 128 - sizeof(struct ec_host_response);
		} else if (i & EC_HOST_CMD_FLAG_LPC_ARGS_SUPPORTED) {
			/* Protocol version 2 */
			priv->ec_command = cros_ec_command_lpc_lm4;
#if 0
			/*
			 * FIXME(dhendrix): We should be able to use
			 * EC_PROTO2_MAX_PARAM_SIZE, but that has not been thoroughly
			 * tested so for now leave the sizes at default.
			 */
			cros_ec.max_data_read = EC_PROTO2_MAX_PARAM_SIZE;
			cros_ec.max_data_write = EC_PROTO2_MAX_PARAM_SIZE;
#endif
		} else {
			priv->ec_command = cros_ec_command_lpc_old;
		}
	}

	/* Try hello command -- for EC only supports API v0
	 * TODO: (crosbug.com/p/33102) Remove after MP.
	 */
	/* reduce timeout period temporarily in case EC is not present */
	ec_timeout_usec = 25000;
	if (cros_ec_test(&cros_ec_lpc_priv))
		return 1;
	ec_timeout_usec = old_timeout;

	cros_ec_set_max_size(&cros_ec_lpc_priv, &cros_ec);

	return 0;
}

static int cros_ec_lpc_shutdown(void *data)
{
#if USE_CROS_EC_LOCK == 1
	release_cros_ec_lock();
#endif
	return 0;
}

int cros_ec_probe_lpc(const char *name) {
	msg_pdbg("%s():%d ...\n", __func__, __LINE__);

	if (alias && alias->type != ALIAS_EC)
		return 1;

	if (cros_ec_parse_param(&cros_ec_lpc_priv))
		return 1;

	if (detect_ec(&cros_ec_lpc_priv))
		return 1;

	msg_pdbg("CROS_EC detected on LPC bus\n");
	cros_ec_lpc_priv.detected = 1;

	if (buses_supported & BUS_SPI) {
		msg_pdbg("%s():%d remove BUS_SPI from buses_supported.\n",
			__func__, __LINE__);
		buses_supported &= ~BUS_SPI;
	}
	register_opaque_programmer(&cros_ec);
	buses_supported |= BUS_LPC;

	if (register_shutdown(cros_ec_lpc_shutdown, NULL)) {
		msg_perr("Cannot register LPC shutdown function.\n");
		return 1;
	}

	return 0;
}
