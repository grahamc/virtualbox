/** @file
 * Shared Clipboard - Common header for host service and guest clients.
 */

/*
 * Copyright (C) 2006-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

#ifndef VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h
#define VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <VBox/VMMDevCoreTypes.h>
#include <VBox/VBoxGuestCoreTypes.h>
#include <VBox/hgcmsvc.h>

/*
 * The mode of operations.
 */
#define VBOX_SHARED_CLIPBOARD_MODE_OFF           0
#define VBOX_SHARED_CLIPBOARD_MODE_HOST_TO_GUEST 1
#define VBOX_SHARED_CLIPBOARD_MODE_GUEST_TO_HOST 2
#define VBOX_SHARED_CLIPBOARD_MODE_BIDIRECTIONAL 3

/*
 * The service functions which are callable by host.
 */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_SET_MODE      1
/** Run headless on the host, i.e. do not touch the host clipboard. */
#define VBOX_SHARED_CLIPBOARD_HOST_FN_SET_HEADLESS  2

/*
 * The service functions which are called by guest.
 */
/** Calls the host and waits (blocking) for an host event VBOX_SHARED_CLIPBOARD_HOST_MSG_*. */
#define VBOX_SHARED_CLIPBOARD_FN_GET_HOST_MSG      1
/** Sends a list of available formats to host. */
#define VBOX_SHARED_CLIPBOARD_FN_REPORT_FORMATS    2
/** Reads data in specified format from host. */
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA         3
/** Writes data in requested format to host. */
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA        4
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA_CHUNK   5
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_CHUNK  6
#define VBOX_SHARED_CLIPBOARD_FN_READ_DATA_HDR     7
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DATA_HDR    8
#define VBOX_SHARED_CLIPBOARD_FN_READ_DIR          9
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_DIR         10
#define VBOX_SHARED_CLIPBOARD_FN_READ_FILE_HDR     11
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_HDR    12
#define VBOX_SHARED_CLIPBOARD_FN_READ_FILE_DATA    13
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_FILE_DATA   14
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_CANCEL      15
#define VBOX_SHARED_CLIPBOARD_FN_WRITE_ERROR       16

#define VBOX_SHARED_CLIPBOARD_MAX_CHUNK_SIZE       _64K

/*
 * HGCM parameter structures.
 */
#pragma pack(1)
typedef struct _VBoxClipboardGetHostMsg
{
    VBGLIOCHGCMCALL hdr;

    /* VBOX_SHARED_CLIPBOARD_HOST_MSG_* */
    HGCMFunctionParameter msg;     /* OUT uint32_t */

    /* VBOX_SHARED_CLIPBOARD_FMT_*, depends on the 'msg'. */
    HGCMFunctionParameter formats; /* OUT uint32_t */
} VBoxClipboardGetHostMsg;

#define VBOX_SHARED_CLIPBOARD_CPARMS_GET_HOST_MSG 2

typedef struct _VBoxClipboardWriteFormats
{
    VBGLIOCHGCMCALL hdr;

    /* VBOX_SHARED_CLIPBOARD_FMT_* */
    HGCMFunctionParameter formats; /* OUT uint32_t */
} VBoxClipboardWriteFormats;

#define VBOX_SHARED_CLIPBOARD_CPARMS_FORMATS 1

typedef struct _VBoxClipboardReadData
{
    VBGLIOCHGCMCALL hdr;

    /* Requested format. */
    HGCMFunctionParameter format; /* IN uint32_t */

    /* The data buffer. */
    HGCMFunctionParameter ptr;    /* IN linear pointer. */

    /* Size of returned data, if > ptr->cb, then no data was
     * actually transferred and the guest must repeat the call.
     */
    HGCMFunctionParameter size;   /* OUT uint32_t */

} VBoxClipboardReadData;

#define VBOX_SHARED_CLIPBOARD_CPARMS_READ_DATA 3

typedef struct _VBoxClipboardWriteData
{
    VBGLIOCHGCMCALL hdr;

    /* Returned format as requested in the VBOX_SHARED_CLIPBOARD_HOST_MSG_READ_DATA message. */
    HGCMFunctionParameter format; /* IN uint32_t */

    /* Data.  */
    HGCMFunctionParameter ptr;    /* IN linear pointer. */
} VBoxClipboardWriteData;

#define VBOX_SHARED_CLIPBOARD_CPARMS_WRITE_DATA 2

#ifdef VBOX_WITH_SHARED_CLIPBOARD_URI_LIST
/**
 * Data header.
 */
typedef struct _VBOXCLIPBOARDDATAHDR
{
    /** Data transfer flags. Not yet used and must be 0. */
    uint32_t                    uFlags;
    /** Screen ID where the data originates from. */
    uint32_t                    uScreenId;
    /** Total size (in bytes) to transfer. */
    uint64_t                    cbTotal;
    /** Meta data size (in bytes) to transfer.
     *  This size also is part of cbTotal already. */
    uint32_t                    cbMeta;
    /** Meta format buffer. */
    void                       *pvMetaFmt;
    /** Size (in bytes) of meta format buffer. */
    uint32_t                    cbMetaFmt;
    /** Number of objects (files/directories) to transfer. */
    uint64_t                    cObjects;
    /** Compression type. Currently unused, so specify 0.
     **@todo Add IPRT compression type enumeration as soon as it's available. */
    uint32_t                    enmCompression;
    /** Checksum type. Currently unused, so specify RTDIGESTTYPE_INVALID. */
    RTDIGESTTYPE                enmChecksumType;
    /** The actual checksum buffer for the entire data to be transferred,
     *  based on enmChksumType. If RTDIGESTTYPE_INVALID is specified,
     *  no checksum is being used and pvChecksum will be NULL. */
    void                       *pvChecksum;
    /** Size (in bytes) of checksum. */
    uint32_t                    cbChecksum;
} VBOXCLIPBOARDDATAHDR, *PVBOXCLIPBOARDDATAHDR;

/**
 * Sends the header of an incoming (meta) data block.
 *
 * Used by:
 * XXX
 */
typedef struct _VBoxClipboardWriteDataHdrMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;        /* OUT uint32_t */
    /** Data transfer flags. Not yet used and must be 0. */
    HGCMFunctionParameter uFlags;          /* OUT uint32_t */
    /** Screen ID where the data originates from. */
    HGCMFunctionParameter uScreenId;       /* OUT uint32_t */
    /** Total size (in bytes) to transfer. */
    HGCMFunctionParameter cbTotal;         /* OUT uint64_t */
    /**
     * Total meta data size (in bytes) to transfer.
     * This size also is part of cbTotal already, so:
     *
     * cbTotal = cbMeta + additional size for files etc.
     */
    HGCMFunctionParameter cbMeta;          /* OUT uint64_t */
    /** Meta data format. */
    HGCMFunctionParameter pvMetaFmt;       /* OUT ptr */
    /** Size (in bytes) of meta data format. */
    HGCMFunctionParameter cbMetaFmt;       /* OUT uint32_t */
    /* Number of objects (files/directories) to transfer. */
    HGCMFunctionParameter cObjects;        /* OUT uint64_t */
    /** Compression type. */
    HGCMFunctionParameter enmCompression;  /* OUT uint32_t */
    /** Checksum type. */
    HGCMFunctionParameter enmChecksumType; /* OUT uint32_t */
    /** Checksum buffer for the entire data to be transferred. */
    HGCMFunctionParameter pvChecksum;      /* OUT ptr */
    /** Size (in bytes) of checksum. */
    HGCMFunctionParameter cbChecksum;      /* OUT uint32_t */
} VBoxClipboardWriteDataHdrMsg;

typedef struct _VBOXCLIPBOARDWRITEDATACHUNK
{
    /** Data block buffer. */
    void                       *pvData;
    /** Size (in bytes) of data block. */
    uint32_t                    cbData;
    /** (Rolling) Checksum. Not yet implemented. */
    void                       *pvChecksum;
    /** Size (in bytes) of checksum. Not yet implemented. */
    uint32_t                    cbChecksum;
} VBOXCLIPBOARDWRITEDATACHUNK, *PVBOXCLIPBOARDWRITEDATACHUNK;

/**
 * Sends a (meta) data block to the host.
 *
 * Used by:
 * GUEST_DND_GH_SND_DATA
 */
typedef struct _VBoxClipboardWriteDataChunkMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Data block to send. */
    HGCMFunctionParameter pvData;       /* OUT ptr */
    /** Size (in bytes) of data block to send. */
    HGCMFunctionParameter cbData;       /* OUT uint32_t */
    /** (Rolling) Checksum, based on checksum type in data header. */
    HGCMFunctionParameter pvChecksum;   /* OUT ptr */
    /** Size (in bytes) of checksum. */
    HGCMFunctionParameter cbChecksum;   /* OUT uint32_t */
} VBoxClipboardWriteDataChunkMsg;

/**
 * Sends a directory entry.
 *
 * Used by:
 * XXX
 */
typedef struct _VBoxClipboardWriteDirMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Directory name. */
    HGCMFunctionParameter pvName;       /* OUT ptr */
    /** Size (in bytes) of directory name. */
    HGCMFunctionParameter cbName;       /* OUT uint32_t */
    /** Directory mode. */
    HGCMFunctionParameter fMode;        /* OUT uint32_t */
} VBoxClipboardWriteDirMsg;

/**
 * File header message, marking the start of transferring a new file.
 *
 * Used by:
 * XXX
 */
typedef struct _VBoxClipboardWriteFileHdrMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** File path. */
    HGCMFunctionParameter pvName;       /* OUT ptr */
    /** Size (in bytes) of file path. */
    HGCMFunctionParameter cbName;       /* OUT uint32_t */
    /** Optional flags; unused at the moment. */
    HGCMFunctionParameter uFlags;       /* OUT uint32_t */
    /** File creation mode. */
    HGCMFunctionParameter fMode;        /* OUT uint32_t */
    /** Total size (in bytes). */
    HGCMFunctionParameter cbTotal;      /* OUT uint64_t */
} VBoxClipboardWriteFileHdrMsg;

/**
 * Sends data of a file entry.
 *
 * Used by:
 * XXX
 */
typedef struct _VBoxClipboardWriteFileDataMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** Current data chunk. */
    HGCMFunctionParameter pvData;       /* OUT ptr */
    /** Size (in bytes) of current data chunk. */
    HGCMFunctionParameter cbData;       /* OUT uint32_t */
    /** Checksum of data block, based on the checksum
     *  type in the data header. Optional. */
    HGCMFunctionParameter pvChecksum;   /* OUT ptr */
    /** Size (in bytes) of curren data chunk checksum. */
    HGCMFunctionParameter cbChecksum;   /* OUT uint32_t */
} VBoxClipboardWriteFileDataMsg;

/**
 * Sends an error event.
 *
 * Used by:
 * XXX
 */
typedef struct _VBoxClipboardWriteErrorMsg
{
    VBGLIOCHGCMCALL hdr;

    /** Context ID. Unused at the moment. */
    HGCMFunctionParameter uContext;     /* OUT uint32_t */
    /** The error code (IPRT-style). */
    HGCMFunctionParameter rc;           /* OUT uint32_t */
} VBoxClipboardWriteErrorMsg;
#endif /* VBOX_WITH_SHARED_CLIPBOARD_URI_LIST */

#pragma pack()

bool VBoxSvcClipboardGetHeadless(void);
bool VBoxSvcClipboardLock(void);
void VBoxSvcClipboardUnlock(void);

#endif /* !VBOX_INCLUDED_HostServices_VBoxClipboardSvc_h */
