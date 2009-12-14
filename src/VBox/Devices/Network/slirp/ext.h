/** $Id$ */
/** @file
 * some externals helpers
 */

/*
 * Copyright (C) 2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

#ifdef VBOX_WITH_SLIRP_BSD_MBUF
# ifndef IN_BSD
#  define zone_mbuf slirp_zone_mbuf(pData)
#  define zone_clust slirp_zone_clust(pData)
#  define zone_pack slirp_zone_pack(pData)
#  define zone_jumbop slirp_zone_jumbop(pData)
#  define zone_jumbo9 slirp_zone_jumbo9(pData)
#  define zone_jumbo16 slirp_zone_jumbo16(pData)
#  define zone_ext_refcnt slirp_zone_ext_refcnt(pData)
static inline uma_zone_t slirp_zone_mbuf(PNATState);
static inline uma_zone_t slirp_zone_clust(PNATState);
static inline uma_zone_t slirp_zone_pack(PNATState);
static inline uma_zone_t slirp_zone_jumbop(PNATState);
static inline uma_zone_t slirp_zone_jumbo9(PNATState);
static inline uma_zone_t slirp_zone_jumbo16(PNATState);
static inline uma_zone_t slirp_zone_ext_refcnt(PNATState);
# else
#  undef zone_mbuf
#  undef zone_clust
#  undef zone_pack
#  undef zone_jumbop
#  undef zone_jumbo9
#  undef zone_jumbo16
#  undef zone_ext_refcnt

#  define zone_mbuf pData->zone_mbuf
#  define zone_clust pData->zone_clust
#  define zone_pack pData->zone_pack
#  define zone_jumbop pData->zone_jumbop
#  define zone_jumbo9 pData->zone_jumbo9
#  define zone_jumbo16 pData->zone_jumbo16
#  define zone_ext_refcnt pData->zone_ext_refcnt
# endif
#endif

#ifndef _EXT_H_
#define _EXT_H_
# define fprintf vbox_slirp_fprintf
# define printf vbox_slirp_printf
#ifndef vbox_slirp_printfV
static void vbox_slirp_printV(char *format, va_list args)
{
# ifdef LOG_ENABLED
    char buffer[1024];
    memset(buffer, 0, 1024);
    RTStrPrintfV(buffer, 1024, format, args);

    Log2(("NAT:EXT: %s\n", buffer));
# endif
}
#endif

#ifndef vbox_slirp_printf
static void vbox_slirp_printf(char *format, ...)
{
# ifdef LOG_ENABLED
    va_list args;
    va_start(args, format);
    vbox_slirp_printV(format, args);
    va_end(args);
# endif
}
#endif

#ifndef vbox_slirp_fprintf
static void vbox_slirp_fprintf(void *ignored, char *format, ...)
{
# ifdef LOG_ENABLED
    va_list args;
    va_start(args, format);
    vbox_slirp_printV(format, args);
    va_end(args);
# endif
}
#endif
#endif
