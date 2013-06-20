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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "gec_dev.h"
#include "file.h"
#include "flash.h"
#include "gec_ec_commands.h"
#include "programmer.h"

#define GEC_DEV_NAME		"/dev/cros_ec"
#define GEC_COMMAND_RETRIES	50

int gec_fd;		/* File descriptor of GEC_DEV_NAME */

/**
 * Wait for a command to complete, then return the response
 *
 * This is called when we get an EAGAIN response from the EC. We need to
 * send EC_CMD_GET_COMMS_STATUS commands until the EC indicates it is
 * finished the command that we originally sent.
 *
 * returns 0 if command is successful, <0 to indicate timeout or error
 */
static int command_wait_for_response(void)
{
	struct ec_response_get_comms_status status;
	struct cros_ec_command cmd;
	int ret;
	int i;

	cmd.version = 0;
	cmd.command = EC_CMD_GET_COMMS_STATUS;
	cmd.outdata = NULL;
	cmd.outsize = 0;
	cmd.indata = (uint8_t *)&status;
	cmd.insize = sizeof(status);

	for (i = 1; i <= GEC_COMMAND_RETRIES; i++) {
		ret = ioctl(gec_fd, CROS_EC_DEV_IOCXCMD, &cmd, sizeof(cmd));
		if (ret) {
			msg_perr("%s(): CrOS EC command failed: %d\n",
				 __func__, ret);
			ret = -EC_RES_ERROR;
			break;
		}

		if (!(status.flags & EC_COMMS_STATUS_PROCESSING)) {
			ret = -EC_RES_SUCCESS;
			break;
		}

		usleep(1000);
	}

	return ret;
}

/*
 * gec_command_dev - Issue command to GEC device
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * This uses the kernel Chrome OS EC driver to communicate with the EC.
 *
 * The outdata and indata buffers contain payload data (if any); command
 * and response codes as well as checksum data are handled transparently by
 * this function.
 *
 * Returns >=0 for success, or negative if other error.
 */
static int gec_command_dev(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	struct cros_ec_command cmd;
	int ret;

	cmd.version = version;
	cmd.command = command;
	cmd.outdata = outdata;
	cmd.outsize = outsize;
	cmd.indata = indata;
	cmd.insize = insize;
	ret = ioctl(gec_fd, CROS_EC_DEV_IOCXCMD, &cmd, sizeof(cmd));
	if (ret < 0 && errno == -EAGAIN)
		ret = command_wait_for_response();

	if (ret) {
		msg_perr("%s(): Transfer failed: %d\n", __func__, ret);
		return -EC_RES_ERROR;
	}

	return cmd.insize;
}

static struct gec_priv gec_dev_priv = {
	.detected	= 0,
	.ec_command	= gec_command_dev,
};

static const struct opaque_programmer opaque_programmer_gec_dev = {
	.max_data_read	= EC_OLD_PARAM_SIZE,
	.max_data_write	= 64,
	.probe		= gec_probe_size,
	.read		= gec_read,
	.write		= gec_write,
	.erase		= gec_block_erase,
	.data		= &gec_dev_priv,
};

int gec_probe_dev(void)
{
	if (alias && alias->type != ALIAS_EC)
		return 1;

	msg_pdbg("%s: probing for GEC at %s\n", __func__, GEC_DEV_NAME);
	gec_fd = open(GEC_DEV_NAME, O_RDWR);
	if (gec_fd < 0)
		return gec_fd;

	msg_pdbg("GEC detected at %s\n", GEC_DEV_NAME);
	register_opaque_programmer(&opaque_programmer_gec_dev);
	gec_dev_priv.detected = 1;

	return 0;
}
