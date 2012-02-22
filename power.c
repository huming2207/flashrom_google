/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2011 The Chromium OS Authors
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
 *
 * power.c: power management routines
 */

#include <stdlib.h>
#include <string.h>

#include "flash.h"	/* for msg_* */
#include "power.h"

enum pm_state {
	PM_OFF,
	PM_ON,
};

/* power management daemons used in Chromium OS */
static struct chromiumos_pm {
	enum pm_state powerd;	/* power management running inside minijail */
	enum pm_state powerm;	/* power management running outside minijail */
} pm_state_at_start;

static int pm_disabled;

/* disable power management and set pm_state_at_start as an output */
int disable_power_management()
{
	if (pm_disabled) {
		msg_pdbg("%s: Nothing to do.\n", __func__);
		return 0;
	}

	msg_pdbg("%s: Disabling power management services.\n", __func__);

	/* if the service terminates successfully, then it was running when
	 * flashrom was invoked */
	if (system("stop powerd >/dev/null 2>&1") == 0)
		pm_state_at_start.powerd = PM_ON;
	else
		pm_state_at_start.powerd = PM_OFF;

	if (system("stop powerm >/dev/null 2>&1") == 0)
		pm_state_at_start.powerm = PM_ON;
	else
		pm_state_at_start.powerm = PM_OFF;

	pm_disabled = 1;
	return 0;
}

/* (re-)enable power management */
int restore_power_management()
{
	int rc = 0;

	if (!pm_disabled) {
		msg_pdbg("%s(): Power management enabled\n", __func__);
		return 0;
	}

	msg_pdbg("%s: (Re-)Enabling power management services\n", __func__);

	if (pm_state_at_start.powerd == PM_ON) {
		if (system("start powerd >/dev/null 2>&1") != 0) {
			msg_perr("Cannot restart powerd service\n");
			rc |= 1;
		}
	}

	if (pm_state_at_start.powerm == PM_ON) {
		if (system("start powerm >/dev/null 2>&1") != 0) {
			msg_perr("Cannot restart powerm service\n");
			rc |= 1;
		}
	}

	pm_disabled = 0;
	return rc;
}
