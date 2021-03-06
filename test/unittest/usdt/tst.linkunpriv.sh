#!/bin/bash
#
# Oracle Linux DTrace.
# Copyright (c) 2006, Oracle and/or its affiliates. All rights reserved.
# Licensed under the Universal Permissive License v 1.0 as shown at
# http://oss.oracle.com/licenses/upl.
#
# @@skip: no privilege support (yet)

if [ $# != 1 ]; then
	echo expected one argument: '<'dtrace-path'>'
	exit 2
fi

dtrace=$1
CC=/usr/bin/gcc
CFLAGS="-I${PWD}/uts/common"

DIRNAME="$tmpdir/usdt-linkunpriv.$$.$RANDOM"
mkdir -p $DIRNAME
cd $DIRNAME

ppriv -s A=basic $$

cat > test.c <<EOF
#include <sys/sdt.h>

int
main(int argc, char **argv)
{
	DTRACE_PROBE(test_prov, zero);
	DTRACE_PROBE1(test_prov, one, 1);
	DTRACE_PROBE2(test_prov, two, 2, 3);
	DTRACE_PROBE3(test_prov, three, 4, 5, 7);
	DTRACE_PROBE4(test_prov, four, 7, 8, 9, 10);
	DTRACE_PROBE5(test_prov, five, 11, 12, 13, 14, 15);
}
EOF

cat > prov.d <<EOF
provider test_prov {
	probe zero();
	probe one(uintptr_t);
	probe two(uintptr_t, uintptr_t);
	probe three(uintptr_t, uintptr_t, uintptr_t);
	probe four(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	probe five(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
};
EOF

${CC} ${CFLAGS} -c test.c
if [ $? -ne 0 ]; then
	echo "failed to compile test.c" >& 2
	exit 1
fi
$dtrace -G -s prov.d test.o
if [ $? -ne 0 ]; then
	echo "failed to create DOF" >& 2
	exit 1
fi
${CC} ${CFLAGS} -o test test.o prov.o
if [ $? -ne 0 ]; then
	echo "failed to link final executable" >& 2
	exit 1
fi

exit 0
