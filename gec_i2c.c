/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 Google Inc.
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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "flash.h"
#include "gec_ec_commands.h"
#include "programmer.h"

#define SYSFS_I2C_DEV_ROOT	"/sys/bus/i2c/devices"
#define GEC_I2C_ADAPTER_NAME	"cros_ec_i2c"
#define GEC_I2C_ADDRESS		0x1e

/* v0 protocol bytes (command/response code + checksum byte) */
#define GEC_PROTO_BYTES_V0	2

/* v1 protocol bytes
 *   OUT: (version, command, size, ... request ..., checksum) */
#define GEC_PROTO_BYTES_V1_OUT	4
/*   IN:  (command, size, ... response ..., checkcum) */
#define GEC_PROTO_BYTES_V1_IN	3

/*
 * Flash erase on a 1024 byte chunk takes about 22.05ms on STM32-based GEC.
 * We'll leave about a half millisecond extra for other overhead to avoid
 * polling the status too aggressively.
 *
 * TODO: determine better delay value or mechanism for all chips and commands.
 */
#define STM32_ERASE_DELAY	22500	/* 22.5ms */
#define GEC_COMMAND_RETRIES	5

static int ec_timeout_usec = 1000000;

static unsigned int bus;

static int gec_i2c_shutdown(void *data)
{
	return linux_i2c_shutdown(data);
}

void delay_for_command(int command)
{
	switch (command) {
	case EC_CMD_FLASH_ERASE:
		programmer_delay(STM32_ERASE_DELAY);
		break;
	default:
		break;
	}
}

/* returns 0 if command is successful, <0 to indicate timeout or error */
static int command_response(int command, int version, uint8_t response_code)
{
	uint8_t *status_cmd;
	struct ec_response_get_comms_status status;
	int i, status_cmd_len, ret = -EC_RES_TIMEOUT;

	if (response_code != EC_RES_IN_PROGRESS)
		return -response_code;

	if (version == 0) {
		status_cmd_len = 1;
		status_cmd = malloc(status_cmd_len);
		status_cmd[0] = EC_CMD_GET_COMMS_STATUS;
	} else {
		int csum;

		status_cmd_len = GEC_PROTO_BYTES_V1_OUT;
		status_cmd = malloc(status_cmd_len);
		status_cmd[0] = EC_CMD_VERSION0 + 1;
		status_cmd[1] = EC_CMD_GET_COMMS_STATUS;
		status_cmd[2] = 0;

		csum = status_cmd[0] + status_cmd[1] + status_cmd[2];
		status_cmd[3] = csum & 0xff;
	}

	for (i = 1; i <= GEC_COMMAND_RETRIES; i++) {
		/*
		 * The first retry might work practically immediately, so
		 * skip the delay for the first retry.
		 */
		if (i != 1)
			delay_for_command(command);

		msg_pspew("retry %d / %d\n", i, GEC_COMMAND_RETRIES);
		ret = linux_i2c_xfer(bus, GEC_I2C_ADDRESS,
				     &status, sizeof(status),
				     status_cmd, status_cmd_len);

		if (ret) {
			msg_perr("%s(): linux_i2c_xfer() failed: %d\n",
				 __func__, ret);
			ret = -EC_RES_ERROR;
			break;
		}

		if (!(status.flags & EC_COMMS_STATUS_PROCESSING)) {
			ret = -EC_RES_SUCCESS;
			break;
		}
	}

	free(status_cmd);
	return ret;
}

/*
 * gec_command_i2c - (v0) Issue command to GEC over I2C
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * Protocol-related details will be handled by this function. The outdata
 * and indata buffers will contain payload data (if any); command and response
 * codes as well as checksum data are handled transparently by this function.
 *
 * Returns the command status code, or -1 if other error.
 */
static int gec_command_i2c_v0(int command, int version,
			      const void *outdata, int outsize,
			      void *indata, int insize) {
	int ret = -1;
	uint8_t *req_buf = NULL, *resp_buf = NULL;
	int req_len = 0, resp_len = 0;
	int i, csum;

	if (version > 0) {
		msg_perr("%s() version >0 not supported yet.\n", __func__);
		return -EC_RES_INVALID_VERSION;
	}

	if (outsize) {
		req_len = outsize + GEC_PROTO_BYTES_V0;
		req_buf = calloc(1, req_len);
		if (!req_buf)
			goto done;

		/* copy message payload and compute checksum */
		memcpy(&req_buf[1], outdata, outsize);
		for (i = 0, csum = 0; i < outsize; i++) {
			req_buf[i + 1] = *((uint8_t *)outdata + i);
			csum += *((uint8_t *)outdata + i);
		}
		req_buf[req_len - 1] = csum & 0xff;
	} else {
		/* request buffer will hold command code only */
		req_len = 1;
		req_buf = calloc(1, req_len);
		if (!req_buf)
			goto done;
	}
	req_buf[0] = command;

	msg_pspew("%s: req_buf: ", __func__);
	for (i = 0; i < req_len; i++)
		msg_pspew("%02x ", req_buf[i]);
	msg_pspew("\n");

	resp_len = insize + GEC_PROTO_BYTES_V0;
	resp_buf = calloc(1, resp_len);
	if (!resp_buf)
		goto done;

	ret = linux_i2c_xfer(bus, GEC_I2C_ADDRESS,
			     resp_buf, resp_len, req_buf, req_len);
	if (ret) {
		msg_perr("%s(): linux_i2c_xfer() failed: %d\n", __func__, ret);
		ret = EC_RES_ERROR;
		goto done;
	}

	msg_pspew("%s: resp_buf: ", __func__);
	for (i = 0; i < resp_len; i++)
		msg_pspew("%02x ", resp_buf[i]);
	msg_pspew("\n");

	ret = command_response(command, version, resp_buf[0]);
	if (ret)
		msg_pdbg("command 0x%02x returned an error %d\n", command, ret);

	if (insize) {
		/* copy response packet payload and compute checksum */
		for (i = 0, csum = 0; i < insize; i++)
			csum += resp_buf[i + 1];
		csum &= 0xff;

		if (csum != resp_buf[resp_len - 1]) {
			msg_pdbg("bad checksum (got 0x%02x from EC, calculated "
				 "0x%02x\n", resp_buf[resp_len - 1], csum);
			ret = -EC_RES_INVALID_CHECKSUM;
			goto done;
		}

		memcpy(indata, &resp_buf[1], insize);
	}
done:
	if (resp_buf)
		free(resp_buf);
	if (req_buf)
		free(req_buf);
	return ret;
}

/*
 * gec_command_i2c - (v1) Issue command to GEC over I2C
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * Protocol-related details will be handled by this function. The outdata
 * and indata buffers will contain payload data (if any); command and response
 * codes as well as checksum data are handled transparently by this function.
 *
 * Returns the command status code, or -1 if other error.
 */
static int gec_command_i2c(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize) {
	int ret = -1;
	uint8_t *req_buf = NULL, *resp_buf = NULL;
	int req_len = 0, resp_len = 0;
	int i, csum;

	if (version == 0)
		return gec_command_i2c_v0(command, version, outdata, outsize,
				          indata, insize);
	else if (version > 1) {
		msg_perr("%s() version >1 not supported yet.\n", __func__);
		return -EC_RES_INVALID_VERSION;
	}

	req_len = outsize + GEC_PROTO_BYTES_V1_OUT;
	req_buf = calloc(1, req_len);
	if (!req_buf)
		goto done;

	req_buf[0] = version + EC_CMD_VERSION0;
	req_buf[1] = command;
	req_buf[2] = outsize;
	csum = req_buf[0] + req_buf[1] + req_buf[2];
	/* copy message payload and compute checksum */
	memcpy(&req_buf[3], outdata, outsize);
	for (i = 0; i < outsize; i++) {
		csum += *((uint8_t *)outdata + i);
	}
	req_buf[req_len - 1] = csum & 0xff;

	msg_pspew("%s: req_buf: ", __func__);
	for (i = 0; i < req_len; i++)
		msg_pspew("%02x ", req_buf[i]);
	msg_pspew("\n");

	resp_len = insize + GEC_PROTO_BYTES_V1_IN;
	resp_buf = calloc(1, resp_len);
	if (!resp_buf)
		goto done;

	ret = linux_i2c_xfer(bus, GEC_I2C_ADDRESS,
			     resp_buf, resp_len, req_buf, req_len);
	if (ret) {
		msg_perr("%s(): linux_i2c_xfer() failed: %d\n", __func__, ret);
		ret = EC_RES_ERROR;
		goto done;
	}

	msg_pspew("%s: resp_buf: ", __func__);
	for (i = 0; i < resp_len; i++)
		msg_pspew("%02x ", resp_buf[i]);
	msg_pspew("\n");

	ret = command_response(command, version, resp_buf[0]);
	if (ret)
		msg_pdbg("command 0x%02x returned an error %d\n", command, ret);
	resp_len = resp_buf[1];

	if (insize) {
		/* copy response packet payload and compute checksum */
		csum = resp_buf[0] + resp_buf[1];
		for (i = 0; i < insize; i++)
			csum += resp_buf[i + 2];
		csum &= 0xff;

		if (csum != resp_buf[resp_len + 2]) {
			msg_pdbg("bad checksum (got 0x%02x from EC, calculated "
				 "0x%02x\n", resp_buf[resp_len + 2], csum);
			ret = -EC_RES_INVALID_CHECKSUM;
			goto done;
		}

		memcpy(indata, &resp_buf[2], insize);
	}
	ret = insize;
done:
	if (resp_buf)
		free(resp_buf);
	if (req_buf)
		free(req_buf);
	return ret;
}

static int detect_ec(void)
{
	struct ec_params_hello request;
	struct ec_response_hello response;
	int rc = 0;
	int old_timeout = ec_timeout_usec;

	/* FIXME: for now we allow BUS_LPC because scripts might rely on
	   passing "-p internal:bus=lpc" to target EC */
	if ((target_bus != BUS_PROG) && (target_bus != BUS_LPC)) {
		msg_pdbg("%s():%d cannot use target_bus\n", __func__, __LINE__);
		return 1;
	}

	/* reduce timeout period temporarily in case EC is not present */
	ec_timeout_usec = 25000;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	msg_pdbg("%s: sending HELLO request with 0x%08x\n",
	         __func__, request.in_data);
	rc = gec_command_i2c(EC_CMD_HELLO, 0, &request,
			     sizeof(request), &response, sizeof(response));
	msg_pdbg("%s: response: 0x%08x\n", __func__, response.out_data);

	ec_timeout_usec = old_timeout;

	if (rc || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
		return 1;
	}

	return 0;
}

static struct gec_priv gec_i2c_priv = {
	.detected	= 0,
	.ec_command	= gec_command_i2c,
};

static const struct opaque_programmer opaque_programmer_gec_i2c = {
	.max_data_read	= EC_OLD_PARAM_SIZE,
	.max_data_write	= 64,
	.probe		= gec_probe_size,
	.read		= gec_read,
	.write		= gec_write,
	.erase		= gec_block_erase,
	.data		= &gec_i2c_priv,
};

int gec_probe_i2c(const char *name)
{
	const char *path, *s, *p;
	int ret = 1;

	msg_pdbg("%s: probing for GEC on I2C...\n", __func__);

	/*
	 * It is possible for the MFD to show up on a different bus than
	 * its I2C adapter. For ChromeOS EC, the I2C passthru adapter piggy-
	 * backs on top of the kernel EC driver. This allows the kernel to
	 * use the MFD while allowing userspace access to the I2C module.
	 *
	 * So for the purposes of finding the correct bus, use the name of
	 * the I2C adapter and not the MFD itself.
	 */
	path = scanft(SYSFS_I2C_DEV_ROOT, "name", GEC_I2C_ADAPTER_NAME, 1);
	if (!path) {
		msg_pdbg("GEC I2C adapter not found\n");
		goto gec_probe_i2c_done;
	}

	/*
	 * i2c-* may show up more than once in the path (especially in the
	 * case of the MFD with passthru I2C adapter), so use whichever
	 * instance shows up last.
	 */
	for (s = path + strlen(path) - 4; s > path; s--) {
		if (!strncmp(s, "i2c-", 4))
			break;
	}

	if ((s == path) || (sscanf(s, "i2c-%u", &bus) != 1)) {
		msg_perr("Unable to parse I2C bus number\n");
		goto gec_probe_i2c_done;
	}

	msg_pdbg("Opening GEC (bus %u, addr 0x%02x)\n", bus, GEC_I2C_ADDRESS);
	if (linux_i2c_open(bus, GEC_I2C_ADDRESS))
		goto gec_probe_i2c_done;

	if (detect_ec()) {
		linux_i2c_close();
		goto gec_probe_i2c_done;
	}

	if (register_shutdown(gec_i2c_shutdown, NULL))
		goto gec_probe_i2c_done;

	msg_pdbg("GEC detected on I2C bus\n");
	register_opaque_programmer(&opaque_programmer_gec_i2c);
	gec_i2c_priv.detected = 1;
	ret = 0;

gec_probe_i2c_done:
	free((void*)path);
	return ret;
}
