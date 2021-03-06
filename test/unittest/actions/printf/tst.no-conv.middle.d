/*
 * Oracle Linux DTrace.
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

/*
 * ASSERTION: The printf action supports a constant format string without
 *	      conversions, when it is an action in the middle.
 *
 * SECTION: Actions/printf()
 */

#pragma D option quiet

BEGIN
{
	trace(1);
	printf(" two ");
	trace(3);
	exit(0);
}
