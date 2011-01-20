/*
 * Copyright (C) 2010 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include "big_lock.h"
#include "locks.h"

#include "ipc_lock.h"

static struct ipc_lock big_lock = IPC_LOCK_INIT(BIGLOCK);

int acquire_big_lock(int timeout_secs)
{
	return acquire_lock(&big_lock, timeout_secs * 1000);
}

int release_big_lock(void)
{
	return release_lock(&big_lock);
}
