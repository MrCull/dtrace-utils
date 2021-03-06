/*
 * Oracle Linux DTrace.
 * Copyright (c) 2006, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

/*
 * ASSERTION:
 * 	Increment operators not supported on all types
 *
 * SECTION: Type and Constant Definitions/Enumerations
 *
 */

#pragma D option quiet

BEGIN
{
	a = "boo";
	c = a++;
	exit(1);
}
