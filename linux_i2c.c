/*
 * Copyright (C) 2012 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name of Google Inc. nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "file.h"
#include "flash.h"
#include "programmer.h"

#define I2C_DEV_PREFIX		"/dev/i2c-"
#define I2C_MAX_ADAPTER		255

static int fd = -1;

int linux_i2c_shutdown(void *data)
{
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
	return 0;
}

int linux_i2c_init(void)
{
	/* TODO: Eventually this should contain things like
	   command-line parsing similar to linux_spi. */
	return 0;
}

int linux_i2c_open(int bus, int addr, int force)
{
	char *dev;
	int ret = 1;
	int path_len = strlen(I2C_DEV_PREFIX) + 4;
	int request = force ? I2C_SLAVE_FORCE : I2C_SLAVE;

	if (bus < 0 || bus > I2C_MAX_ADAPTER) {
		msg_perr("Invalid I2C bus %d\n", bus);
		goto linux_i2c_open_done;
	}

	dev = malloc(path_len);
	if (!dev)
		return 1;

	snprintf(dev, path_len, "%s%d", I2C_DEV_PREFIX, bus);
	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		msg_perr("Unable to open I2C device %s: %s\n",
		         dev, strerror(errno));
		goto linux_i2c_open_done;
	}

#if defined (__linux__)
	if (ioctl(fd, request, addr) < 0) {
		msg_perr("Unable to set I2C slave address to 0x%02x\n", addr);
		close(fd);
		goto linux_i2c_open_done;
	}
#else
	ret = -ENOSYS;
#endif

	ret = 0;
linux_i2c_open_done:
	free(dev);
	return ret;
}

void linux_i2c_close(void)
{
	close(fd);
}

/* returns <0 to indicate failure */
int linux_i2c_xfer(int bus, int addr, const void *inbuf,
		   int insize, const void *outbuf, int outsize)
{
	int ret = -1;
	struct i2c_rdwr_ioctl_data data;
	struct i2c_msg *msg = NULL;

	data.nmsgs = 0;

	if (outsize) {
		msg = realloc(msg, sizeof(*msg) * (data.nmsgs + 1));
		msg[data.nmsgs].addr = addr;
		msg[data.nmsgs].flags = 0;
		msg[data.nmsgs].len = outsize;
		msg[data.nmsgs].buf = (void *)outbuf;
		data.nmsgs++;
	}

	if (insize) {
		msg = realloc(msg, sizeof(*msg) * (data.nmsgs + 1));
		msg[data.nmsgs].addr = addr;
		msg[data.nmsgs].flags = I2C_M_RD;
		msg[data.nmsgs].len = insize;
		msg[data.nmsgs].buf = (void *)inbuf;
		data.nmsgs++;
	}

	data.msgs = msg;
	/* send command to EC and read answer */
#if defined (__linux__)
	/* ioctl returns negative errno, else the number of messages executed */
	ret = ioctl(fd, I2C_RDWR, &data);
#else
	ret = -ENOSYS;
#endif

	if (ret != data.nmsgs) {
		msg_perr("i2c transfer failed: %d (err: %d, %s)\n",
		         ret, errno, strerror(errno));
	} else {
		ret = 0;
	}

	free(msg);
	return ret;
}
