/** @file
 *
 * VBox USBController COM Class declaration.
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

#ifndef ____H_USBCONTROLLERIMPL
#define ____H_USBCONTROLLERIMPL

#include "VirtualBoxBase.h"
#include "USBDeviceFilterImpl.h"

#include <VBox/cfgldr.h>

#include <list>

class Machine;
class HostUSBDevice;

class ATL_NO_VTABLE USBController :
    public VirtualBoxBaseWithChildren,
    public VirtualBoxSupportErrorInfoImpl<USBController, IUSBController>,
    public VirtualBoxSupportTranslation<USBController>,
    public IUSBController
{
private:

    struct Data
    {
        /* Constructor. */
        Data() : m_fEnabled(FALSE) { }

        bool operator== (const Data &that) const
        {
            return this == &that || m_fEnabled == that.m_fEnabled;
        }

        /** Enabled indicator. */
        BOOL                            m_fEnabled;
    };

public:

    DECLARE_NOT_AGGREGATABLE(USBController)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(USBController)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
        COM_INTERFACE_ENTRY(IUSBController)
    END_COM_MAP()

    NS_DECL_ISUPPORTS

    HRESULT FinalConstruct();
    void FinalRelease();

    // public initializer/uninitializer for internal purposes only
    HRESULT init (Machine *parent);
    HRESULT init (Machine *parent, USBController *that);
    HRESULT initCopy (Machine *parent, USBController *that);
    void uninit();

    // IUSBController properties
    STDMETHOD(COMGETTER(Enabled))(BOOL *a_pfEnabled);
    STDMETHOD(COMSETTER(Enabled))(BOOL a_fEnabled);
    STDMETHOD(COMGETTER(USBStandard))(USHORT *a_pusUSBStandard);
    STDMETHOD(COMGETTER(DeviceFilters))(IUSBDeviceFilterCollection **aDevicesFilters);

    // IUSBController methods
    STDMETHOD(CreateDeviceFilter) (INPTR BSTR aName, IUSBDeviceFilter **aFilter);
    STDMETHOD(InsertDeviceFilter) (ULONG aPosition, IUSBDeviceFilter *aFilter);
    STDMETHOD(RemoveDeviceFilter) (ULONG aPosition, IUSBDeviceFilter **aFilter);

    // public methods only for internal purposes

    ComObjPtr <Machine, ComWeakRef> &parent() { return m_Parent; };

    HRESULT loadSettings (CFGNODE aMachine);
    HRESULT saveSettings (CFGNODE aMachine);

    bool isModified();
    bool isReallyModified();
    bool rollback();
    void commit();
    void copyFrom (USBController *aThat);

    const Backupable<Data> &data() { return m_Data; }

    HRESULT onMachineRegistered (BOOL aRegistered);

    HRESULT onDeviceFilterChange (USBDeviceFilter *aFilter,
                                  BOOL aActiveChanged = FALSE);

    bool hasMatchingFilter (ComObjPtr <HostUSBDevice> &aDevice);
    bool hasMatchingFilter (IUSBDevice *aUSBDevice);

    // for VirtualBoxSupportErrorInfoImpl
    static const wchar_t *getComponentName() { return L"USBController"; }

private:

    /** specialization for IUSBDeviceFilter */
    ComObjPtr <USBDeviceFilter> getDependentChild (IUSBDeviceFilter *aFilter)
    {
        VirtualBoxBase *child = VirtualBoxBaseWithChildren::
                                getDependentChild (ComPtr <IUnknown> (aFilter));
        return child ? static_cast <USBDeviceFilter *> (child)
                     : NULL;
    }

    void printList();

    /** Parent object. */
    ComObjPtr<Machine, ComWeakRef>  m_Parent;
    /** Peer object. */
    ComObjPtr<USBController>        m_Peer;
    /** Data. */
    Backupable<Data>                m_Data;

    // the following fields need special backup/rollback/commit handling,
    // so they cannot be a part of Data

    typedef std::list <ComObjPtr <USBDeviceFilter> > DeviceFilterList;
    Backupable <DeviceFilterList> m_DeviceFilters;
};

#endif //!____H_USBCONTROLLERIMPL
