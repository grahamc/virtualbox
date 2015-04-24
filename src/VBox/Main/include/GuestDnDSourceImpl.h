/* $Id$ */
/** @file
 * VBox Console COM Class implementation - Guest drag'n drop source.
 */

/*
 * Copyright (C) 2014-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ____H_GUESTDNDSOURCEIMPL
#define ____H_GUESTDNDSOURCEIMPL

#include "GuestDnDSourceWrap.h"
#include "GuestDnDPrivate.h"

struct RECVDATACTX;
typedef struct RECVDATACTX *PRECVDATACTX;

class ATL_NO_VTABLE GuestDnDSource :
    public GuestDnDSourceWrap,
    public GuestDnDBase
{
public:
    /** @name COM and internal init/term/mapping cruft.
     * @{ */
    DECLARE_EMPTY_CTOR_DTOR(GuestDnDSource)

    int     init(const ComObjPtr<Guest>& pGuest);
    void    uninit(void);

    HRESULT FinalConstruct(void);
    void    FinalRelease(void);
    /** @}  */

private:

    /** Private wrapped @name IDnDBase methods.
     * @{ */
    HRESULT isFormatSupported(const com::Utf8Str &aFormat, BOOL *aSupported);
    HRESULT getFormats(std::vector<com::Utf8Str> &aFormats);
    HRESULT addFormats(const std::vector<com::Utf8Str> &aFormats);
    HRESULT removeFormats(const std::vector<com::Utf8Str> &aFormats);

    HRESULT getProtocolVersion(ULONG *aProtocolVersion);
    /** @}  */

    /** Private wrapped @name IDnDSource methods.
     * @{ */
    HRESULT dragIsPending(ULONG uScreenId, std::vector<com::Utf8Str> &aFormats, std::vector<DnDAction_T> &aAllowedActions, DnDAction_T *aDefaultAction);
    HRESULT drop(const com::Utf8Str &aFormat, DnDAction_T aAction, ComPtr<IProgress> &aProgress);
    HRESULT receiveData(std::vector<BYTE> &aData);
    /** @}  */

protected:

#ifdef VBOX_WITH_DRAG_AND_DROP_GH
    /** @name Dispatch handlers for the HGCM callbacks.
     * @{ */
    int i_onReceiveData(PRECVDATACTX pCtx, const void *pvData, uint32_t cbData, uint64_t cbTotalSize);
    int i_onReceiveDir(PRECVDATACTX pCtx, const char *pszPath, uint32_t cbPath, uint32_t fMode);
    int i_onReceiveFileHdr(PRECVDATACTX pCtx, const char *pszPath, uint32_t cbPath, uint64_t cbSize, uint32_t fMode, uint32_t fFlags);
    int i_onReceiveFileData(PRECVDATACTX pCtx, const void *pvData, uint32_t cbData);
    /** @}  */
#endif

protected:

    static DECLCALLBACK(int) i_receiveDataThread(RTTHREAD Thread, void *pvUser);

    /** @name Callbacks for dispatch handler.
     * @{ */
    static DECLCALLBACK(int) i_receiveRawDataCallback(uint32_t uMsg, void *pvParms, size_t cbParms, void *pvUser);
    static DECLCALLBACK(int) i_receiveURIDataCallback(uint32_t uMsg, void *pvParms, size_t cbParms, void *pvUser);
    /** @}  */

protected:

    int i_receiveData(PRECVDATACTX pCtx);
    int i_receiveRawData(PRECVDATACTX pCtx);
    int i_receiveURIData(PRECVDATACTX pCtx);
};

#endif /* !____H_GUESTDNDSOURCEIMPL */

