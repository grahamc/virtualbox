/* $Id$ */
/** @file
 * InnoTek Portable Runtime - Ring-0 logging.
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

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/log.h>
#include <VBox/sup.h>


#if defined _MSC_VER
__declspec(naked)
void RTLogPrintf(const char *pszFormat, ...)
{
    /* damned jump tables. */
    __asm jmp dword ptr [SUPR0Printf];
}
/* EXTREMELY WRONG! KILL THIS FILE!
#elif defined __GNUC__
asm (".globl RTLogPrintf; RTLogPrintf: jmp *SUPR0Printf");
*/
#else
#error "not ported to your compiler yet."
#endif

