/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2006 Oracle, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* @@timeout: 15 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#pragma D option switchrate=100hz
#pragma D option destructive

sched:::on-cpu
/pid == $pid/
{
	self->on++;
}

sched:::off-cpu
/pid == $pid && self->on/
{
	self->off++;
}

sched:::off-cpu
/self->on > 50 && self->off > 50/
{
	exit(0);
}

profile:::tick-1sec
/n++ > 10/
{
	exit(1);
}
