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
 * Copyright 2013 Oracle, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _I386_PLATFORM_H
#define _I386_PLATFORM_H

#include <inttypes.h>
#include <sys/reg.h>

/*
 * Must be no larger than an 'unsigned long'.
 */

const static unsigned char plat_bkpt[] = { 0xcc };

/*
 * Adjustment needed to a trapped process's instruction pointer to yield the
 * address of the breakpoint it trapped on.
 */
const static intptr_t plat_trap_ip_adjust = -1;

/*
 * Name of this platform's instruction pointer.
 */
#define PLAT_IP RIP

#endif
