/** @file
 *
 * VirtualBox COM class implementation
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

#ifndef ____H_HOSTFLOPPYDRIVEIMPL
#define ____H_HOSTFLOPPYDRIVEIMPL

#include "VirtualBoxBase.h"
#include "Collection.h"

class ATL_NO_VTABLE HostFloppyDrive :
    public VirtualBoxSupportErrorInfoImpl <HostFloppyDrive, IHostFloppyDrive>,
    public VirtualBoxSupportTranslation <HostFloppyDrive>,
    public VirtualBoxBase,
    public IHostFloppyDrive
{
public:

    HostFloppyDrive();
    virtual ~HostFloppyDrive();

    DECLARE_NOT_AGGREGATABLE(HostFloppyDrive)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(HostFloppyDrive)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
        COM_INTERFACE_ENTRY(IHostFloppyDrive)
    END_COM_MAP()

    NS_DECL_ISUPPORTS

    // public initializer/uninitializer for internal purposes only
    HRESULT init (INPTR BSTR driveName);

    // IHostDVDDrive properties
    STDMETHOD(COMGETTER(Name)) (BSTR *driveName);

    // public methods for internal purposes only

    const Bstr &driveName() const { return mDriveName; }

    // for VirtualBoxSupportErrorInfoImpl
    static const wchar_t *getComponentName() { return L"HostFloppyDrive"; }

private:

    Bstr mDriveName;
};

COM_DECL_READONLY_ENUM_AND_COLLECTION_BEGIN (HostFloppyDrive)

    STDMETHOD(FindByName) (INPTR BSTR name, IHostFloppyDrive **drive)
    {
        if (!name)
            return E_INVALIDARG;
        if (!drive)
            return E_POINTER;

        *drive = NULL;
        Vector::value_type found;
        Vector::iterator it = vec.begin();
        while (it != vec.end() && !found)
        {
            Bstr n;
            (*it)->COMGETTER(Name) (n.asOutParam());
            if (n == name)
                found = *it;
            ++ it;
        }

        if (!found)
            return setError (E_INVALIDARG, HostFloppyDriveCollection::tr (
                "The host floppy drive named '%ls' could not be found"), name);

        return found.queryInterfaceTo (drive);
    }

COM_DECL_READONLY_ENUM_AND_COLLECTION_END (HostFloppyDrive)

#endif // ____H_HOSTFLOPPYDRIVEIMPL
