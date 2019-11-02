/*
 * Oracle Linux DTrace.
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

#ifndef	_DT_PR_REGS_H
#define	_DT_PR_REGS_H

#include <sys/ptrace.h>
#include <asm/ptrace.h>

#ifdef	__cplusplus
extern "C" {
#endif

#if defined(__amd64)
# define PT_REGS_ARG0		offsetof(struct pt_regs, rdi)
# define PT_REGS_ARG1		offsetof(struct pt_regs, rsi)
# define PT_REGS_ARG2		offsetof(struct pt_regs, rdx)
# define PT_REGS_ARG3		offsetof(struct pt_regs, rcx)
# define PT_REGS_ARG4		offsetof(struct pt_regs, r8)
# define PT_REGS_ARG5		offsetof(struct pt_regs, r9)
#elif defined(__aarch64__)
# define PT_REGS_ARG0		offsetof(struct user_pt_regs, regs[0])
# define PT_REGS_ARG1		offsetof(struct user_pt_regs, regs[1])
# define PT_REGS_ARG2		offsetof(struct user_pt_regs, regs[2])
# define PT_REGS_ARG3		offsetof(struct user_pt_regs, regs[3])
# define PT_REGS_ARG4		offsetof(struct user_pt_regs, regs[4])
# define PT_REGS_ARG5		offsetof(struct user_pt_regs, regs[5])
#endif

#ifdef	__cplusplus
}
#endif

#endif	/* _DT_PR_REGS_H */