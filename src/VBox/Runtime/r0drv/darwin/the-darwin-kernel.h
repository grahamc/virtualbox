/* $Id$ */
/** @file
 * InnoTek Portable Runtime - Include all necessary headers for the Darwing kernel.
 */

/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 */

#ifndef __the_darwin_kernel_h__
#define __the_darwin_kernel_h__

/* Problematic header(s) containing conflicts with IPRT  first. */
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <sys/param.h>
#include <mach/vm_param.h>
#undef ALIGN
#undef MIN
#undef MAX
#undef PAGE_SIZE
#undef PAGE_SHIFT

/* Include the IPRT definitions of the conflicting #defines & typedefs. */
#include <iprt/cdefs.h>
#include <iprt/types.h>
#include <iprt/param.h>


/* After including cdefs, we can check that this really is Darwin. */
#ifndef __DARWIN__
# error "__DARWIN__ must be defined!"
#endif

/* now we're ready for including the rest of the Darwin headers. */
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOLib.h>


__BEGIN_DECLS
kern_return_t thread_terminate(thread_t);
__END_DECLS


/*
 * Internals of the Darwin Ring-0 IPRT.
 */

__BEGIN_DECLS
extern lck_grp_t *g_pDarwinLockGroup;
__END_DECLS


/**
 * Converts from nanoseconds to Darwin absolute time units.
 * @returns Darwin absolute time.
 * @param   u64Nano     Time interval in nanoseconds
 */
DECLINLINE(uint64_t) rtDarwinAbsTimeFromNano(const uint64_t u64Nano)
{
    uint64_t u64AbsTime;
    nanoseconds_to_absolutetime(u64Nano, &u64AbsTime);
    return u64AbsTime;
}


#include <iprt/err.h>

/**
 * Convert from mach kernel return code to IPRT status code.
 * @todo put this where it belongs! (i.e. in a separate file and prototype in iprt/err.h)
 */
DECLINLINE(int) RTErrConvertFromMachKernReturn(kern_return_t rc)
{
    switch (rc)
    {
        case KERN_SUCCESS:      return VINF_SUCCESS;
        default:                return VERR_GENERAL_FAILURE;
    }
}

#endif

