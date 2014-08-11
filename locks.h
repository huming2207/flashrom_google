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
 *
 * locks.h: locks are used to preserve atomicity of operations.
 */

#ifndef LOCKS_H__
#define LOCKS_H__

/* this is the base key, since we have to pick something global */
#define IPC_LOCK_KEY	(0x67736c00 & 0xfffffc00) /* 22 bits "gsl" */

/* The ordering of the following keys matters a lot. We don't want to reorder
 * keys and have a new binary dependent on deployed/used because it will break
 * atomicity of existing users and binaries. In other words, DO NOT REORDER. */

/* this is the "big lock". */
#define BIGLOCK		(IPC_LOCK_KEY + 0)

/* for Google EC */
#define CROS_EC_LOCK		(IPC_LOCK_KEY + 1)

#endif /* LOCKS_H__ */
