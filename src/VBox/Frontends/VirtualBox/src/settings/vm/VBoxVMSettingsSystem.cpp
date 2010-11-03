/* $Id$ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * VBoxVMSettingsSystem class implementation
 */

/*
 * Copyright (C) 2008-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Local includes */
#include "QIWidgetValidator.h"
#include "UIIconPool.h"
#include "VBoxGlobal.h"
#include "VBoxVMSettingsSystem.h"

/* Global includes */
#include <iprt/cdefs.h>
#include <QHeaderView>

#define ITEM_TYPE_ROLE Qt::UserRole + 1

VBoxVMSettingsSystem::VBoxVMSettingsSystem()
    : mValidator(0)
    , mMinGuestCPU(0), mMaxGuestCPU(0)
{
    /* Apply UI decorations */
    Ui::VBoxVMSettingsSystem::setupUi (this);

    /* Setup constants */
    CSystemProperties properties = vboxGlobal().virtualBox().GetSystemProperties();
    uint hostCPUs = vboxGlobal().virtualBox().GetHost().GetProcessorCount();
    mMinGuestCPU = properties.GetMinGuestCPUCount();
    mMaxGuestCPU = RT_MIN (2 * hostCPUs, properties.GetMaxGuestCPUCount());

    /* Populate possible boot items list.
     * Currently, it seems, we are supporting only 4 possible boot device types:
     * 1. Floppy, 2. DVD-ROM, 3. Hard Disk, 4. Network.
     * But maximum boot devices count supported by machine
     * should be retreived through the ISystemProperties getter.
     * Moreover, possible boot device types are not listed in some separate Main vector,
     * so we should get them (randomely?) from the list of all device types.
     * Until there will be separate Main getter for list of supported boot device types,
     * this list will be hard-coded here... */
    int iPossibleBootListSize = qMin((ULONG)4, properties.GetMaxBootPosition());
    for (int iBootPosition = 1; iBootPosition <= iPossibleBootListSize; ++iBootPosition)
    {
        switch (iBootPosition)
        {
            case 1:
                m_possibleBootItems << KDeviceType_Floppy;
                break;
            case 2:
                m_possibleBootItems << KDeviceType_DVD;
                break;
            case 3:
                m_possibleBootItems << KDeviceType_HardDisk;
                break;
            case 4:
                m_possibleBootItems << KDeviceType_Network;
                break;
            default:
                break;
        }
    }

    /* Setup validators */
    mLeMemory->setValidator (new QIntValidator (mSlMemory->minRAM(), mSlMemory->maxRAM(), this));
    mLeCPU->setValidator (new QIntValidator (mMinGuestCPU, mMaxGuestCPU, this));

    /* Setup connections */
    connect (mSlMemory, SIGNAL (valueChanged (int)),
             this, SLOT (valueChangedRAM (int)));
    connect (mLeMemory, SIGNAL (textChanged (const QString&)),
             this, SLOT (textChangedRAM (const QString&)));

    connect (mTbBootItemUp, SIGNAL (clicked()),
             this, SLOT (moveBootItemUp()));
    connect (mTbBootItemDown, SIGNAL (clicked()),
             this, SLOT (moveBootItemDown()));
    connect (mTwBootOrder, SIGNAL (moveItemUp()),
             this, SLOT (moveBootItemUp()));
    connect (mTwBootOrder, SIGNAL (moveItemDown()),
             this, SLOT (moveBootItemDown()));
    connect (mTwBootOrder, SIGNAL (itemToggled()),
             this, SIGNAL (tableChanged()));
    connect (mTwBootOrder, SIGNAL (currentItemChanged (QTreeWidgetItem*,
                                                       QTreeWidgetItem*)),
             this, SLOT (onCurrentBootItemChanged (QTreeWidgetItem*,
                                                   QTreeWidgetItem*)));

    connect (mSlCPU, SIGNAL (valueChanged (int)),
             this, SLOT (valueChangedCPU (int)));
    connect (mLeCPU, SIGNAL (textChanged (const QString&)),
             this, SLOT (textChangedCPU (const QString&)));

    /* Setup iconsets */
    mTbBootItemUp->setIcon(UIIconPool::iconSet(":/list_moveup_16px.png",
                                               ":/list_moveup_disabled_16px.png"));
    mTbBootItemDown->setIcon(UIIconPool::iconSet(":/list_movedown_16px.png",
                                                 ":/list_movedown_disabled_16px.png"));

#ifdef Q_WS_MAC
    /* We need a little space for the focus rect. */
    mLtBootOrder->setContentsMargins (3, 3, 3, 3);
    mLtBootOrder->setSpacing (3);
#endif /* Q_WS_MAC */

    /* Limit min/max. size of QLineEdit */
    mLeMemory->setFixedWidthByText (QString().fill ('8', 5));
    /* Ensure mLeMemory value and validation is updated */
    valueChangedRAM (mSlMemory->value());

    /* Setup cpu slider */
    mSlCPU->setPageStep (1);
    mSlCPU->setSingleStep (1);
    mSlCPU->setTickInterval (1);
    /* Setup the scale so that ticks are at page step boundaries */
    mSlCPU->setMinimum (mMinGuestCPU);
    mSlCPU->setMaximum (mMaxGuestCPU);
    mSlCPU->setOptimalHint (1, hostCPUs);
    mSlCPU->setWarningHint (hostCPUs, mMaxGuestCPU);
    /* Limit min/max. size of QLineEdit */
    mLeCPU->setFixedWidthByText (QString().fill ('8', 3));
    /* Ensure mLeMemory value and validation is updated */
    valueChangedCPU (mSlCPU->value());
    /* Populate chipset combo: */
    mCbChipset->insertItem(0, vboxGlobal().toString(KChipsetType_PIIX3), QVariant(KChipsetType_PIIX3));
    mCbChipset->insertItem(1, vboxGlobal().toString(KChipsetType_ICH9), QVariant(KChipsetType_ICH9));

    /* Install global event filter */
    qApp->installEventFilter (this);

    /* Applying language settings */
    retranslateUi();
}

bool VBoxVMSettingsSystem::isHWVirtExEnabled() const
{
    return mCbVirt->isChecked();
}

int VBoxVMSettingsSystem::cpuCount() const
{
    return mSlCPU->value();
}

bool VBoxVMSettingsSystem::isHIDEnabled() const
{
    return mCbUseAbsHID->isChecked();
}

/* Load data to cashe from corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void VBoxVMSettingsSystem::loadToCacheFrom(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Fill internal variables with corresponding values: */
    /* Load boot-items of current VM: */
    QList<KDeviceType> usedBootItems;
    for (int i = 1; i <= m_possibleBootItems.size(); ++i)
    {
        KDeviceType type = m_machine.GetBootOrder(i);
        if (type != KDeviceType_Null)
        {
            usedBootItems << type;
            UIBootItemData data;
            data.m_type = type;
            data.m_fEnabled = true;
            m_cache.m_bootItems << data;
        }
    }
    /* Load other unique boot-items: */
    for (int i = 0; i < m_possibleBootItems.size(); ++i)
    {
        KDeviceType type = m_possibleBootItems[i];
        if (!usedBootItems.contains(type))
        {
            UIBootItemData data;
            data.m_type = type;
            data.m_fEnabled = false;
            m_cache.m_bootItems << data;
        }
    }
    m_cache.m_fPFHwVirtExSupported = vboxGlobal().virtualBox().GetHost().GetProcessorFeature(KProcessorFeature_HWVirtEx);
    m_cache.m_fPFPAESupported = vboxGlobal().virtualBox().GetHost().GetProcessorFeature(KProcessorFeature_PAE);
    m_cache.m_fIoApicEnabled = m_machine.GetBIOSSettings().GetIOAPICEnabled();
    m_cache.m_fEFIEnabled = m_machine.GetFirmwareType() >= KFirmwareType_EFI && m_machine.GetFirmwareType() <= KFirmwareType_EFIDUAL;
    m_cache.m_fUTCEnabled = m_machine.GetRTCUseUTC();
    m_cache.m_fUseAbsHID = m_machine.GetPointingHidType() == KPointingHidType_USBTablet;
    m_cache.m_fPAEEnabled = m_machine.GetCPUProperty(KCPUPropertyType_PAE);
    m_cache.m_fHwVirtExEnabled = m_machine.GetHWVirtExProperty(KHWVirtExPropertyType_Enabled);
    m_cache.m_fNestedPagingEnabled = m_machine.GetHWVirtExProperty(KHWVirtExPropertyType_NestedPaging);
    m_cache.m_iRAMSize = m_machine.GetMemorySize();
    m_cache.m_cCPUCount = m_cache.m_fPFHwVirtExSupported ? m_machine.GetCPUCount() : 1;
    m_cache.m_chipsetType = m_machine.GetChipsetType();

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

/* Load data to corresponding widgets from cache,
 * this task SHOULD be performed in GUI thread only: */
void VBoxVMSettingsSystem::getFromCache()
{
    /* Apply internal variables data to QWidget(s): */
    for (int i = 0; i < m_cache.m_bootItems.size(); ++i)
    {
        UIBootItemData data = m_cache.m_bootItems[i];
        QString name = vboxGlobal().toString(data.m_type);
        QTreeWidgetItem *pItem = new QTreeWidgetItem(QStringList(name));
        pItem->setData(0, ITEM_TYPE_ROLE, QVariant(data.m_type));
        pItem->setCheckState(0, data.m_fEnabled ? Qt::Checked : Qt::Unchecked);
        mTwBootOrder->addTopLevelItem(pItem);
    }
    mCbApic->setChecked(m_cache.m_fIoApicEnabled);
    mCbEFI->setChecked(m_cache.m_fEFIEnabled);
    mCbTCUseUTC->setChecked(m_cache.m_fUTCEnabled);
    mCbUseAbsHID->setChecked(m_cache.m_fUseAbsHID);
    mSlCPU->setEnabled(m_cache.m_fPFHwVirtExSupported);
    mLeCPU->setEnabled(m_cache.m_fPFHwVirtExSupported);
    mCbPae->setEnabled(m_cache.m_fPFPAESupported);
    mCbPae->setChecked(m_cache.m_fPAEEnabled);
    mCbVirt->setEnabled(m_cache.m_fPFHwVirtExSupported);
    mCbVirt->setChecked(m_cache.m_fHwVirtExEnabled);
    mCbNestedPaging->setEnabled(m_cache.m_fPFHwVirtExSupported && m_cache.m_fHwVirtExEnabled);
    mCbNestedPaging->setChecked(m_cache.m_fNestedPagingEnabled);
    mSlMemory->setValue(m_cache.m_iRAMSize);
    mSlCPU->setValue(m_cache.m_cCPUCount);
    int iChipsetPositionPos = mCbChipset->findData(m_cache.m_chipsetType);
    mCbChipset->setCurrentIndex(iChipsetPositionPos == -1 ? 0 : iChipsetPositionPos);
    adjustBootOrderTWSize();
    if (!m_cache.m_fPFHwVirtExSupported)
        mTwSystem->removeTab(2);

    /* Revalidate if possible: */
    if (mValidator) mValidator->revalidate();
}

/* Save data from corresponding widgets to cache,
 * this task SHOULD be performed in GUI thread only: */
void VBoxVMSettingsSystem::putToCache()
{
    /* Gather internal variables data from QWidget(s): */
    m_cache.m_bootItems.clear();
    for (int i = 0; i < mTwBootOrder->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *pItem = mTwBootOrder->topLevelItem(i);
        UIBootItemData data;
        data.m_type = (KDeviceType)pItem->data(0, ITEM_TYPE_ROLE).toInt();
        data.m_fEnabled = pItem->checkState(0) == Qt::Checked;
        m_cache.m_bootItems << data;
    }
    m_cache.m_fIoApicEnabled = mCbApic->isChecked() || mSlCPU->value() > 1 ||
                               (KChipsetType)mCbChipset->itemData(mCbChipset->currentIndex()).toInt() == KChipsetType_ICH9;
    m_cache.m_fEFIEnabled = mCbEFI->isChecked();
    m_cache.m_fUTCEnabled = mCbTCUseUTC->isChecked();
    m_cache.m_fUseAbsHID = mCbUseAbsHID->isChecked();
    m_cache.m_fPAEEnabled = mCbPae->isChecked();
    m_cache.m_fHwVirtExEnabled = mCbVirt->checkState() == Qt::Checked || mSlCPU->value() > 1;
    m_cache.m_fNestedPagingEnabled = mCbNestedPaging->isChecked();
    m_cache.m_iRAMSize = mSlMemory->value();
    m_cache.m_cCPUCount = mSlCPU->value();
    m_cache.m_chipsetType = (KChipsetType)mCbChipset->itemData(mCbChipset->currentIndex()).toInt();
}

/* Save data from cache to corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void VBoxVMSettingsSystem::saveFromCacheTo(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Gather corresponding values from internal variables: */
    int iBootIndex = 0;
    /* Save boot-items of current VM: */
    for (int i = 0; i < m_cache.m_bootItems.size(); ++i)
    {
        if (m_cache.m_bootItems[i].m_fEnabled)
            m_machine.SetBootOrder(++iBootIndex, m_cache.m_bootItems[i].m_type);
    }
    /* Save other unique boot-items: */
    for (int i = 0; i < m_cache.m_bootItems.size(); ++i)
    {
        if (!m_cache.m_bootItems[i].m_fEnabled)
            m_machine.SetBootOrder(++iBootIndex, KDeviceType_Null);
    }
    m_machine.GetBIOSSettings().SetIOAPICEnabled(m_cache.m_fIoApicEnabled);
    m_machine.SetFirmwareType(m_cache.m_fEFIEnabled ? KFirmwareType_EFI : KFirmwareType_BIOS);
    m_machine.SetRTCUseUTC(m_cache.m_fUTCEnabled);
    m_machine.SetPointingHidType(m_cache.m_fUseAbsHID ? KPointingHidType_USBTablet : KPointingHidType_PS2Mouse);
    m_machine.SetCPUProperty(KCPUPropertyType_PAE, m_cache.m_fPAEEnabled);
    m_machine.SetHWVirtExProperty(KHWVirtExPropertyType_Enabled, m_cache.m_fHwVirtExEnabled);
    m_machine.SetHWVirtExProperty(KHWVirtExPropertyType_NestedPaging, m_cache.m_fNestedPagingEnabled);
    m_machine.SetMemorySize(m_cache.m_iRAMSize);
    m_machine.SetCPUCount(m_cache.m_cCPUCount);
    m_machine.SetChipsetType(m_cache.m_chipsetType);

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

void VBoxVMSettingsSystem::setValidator (QIWidgetValidator *aVal)
{
    mValidator = aVal;
    connect (mCbApic, SIGNAL (stateChanged (int)), mValidator, SLOT (revalidate()));
    connect (mCbVirt, SIGNAL (stateChanged (int)), mValidator, SLOT (revalidate()));
    connect (mCbUseAbsHID, SIGNAL (stateChanged (int)), mValidator, SLOT (revalidate()));
    connect(mCbChipset, SIGNAL(currentIndexChanged(int)), mValidator, SLOT(revalidate()));
}

bool VBoxVMSettingsSystem::revalidate (QString &aWarning, QString & /* aTitle */)
{
    ulong fullSize = vboxGlobal().virtualBox().GetHost().GetMemorySize();
    if (mSlMemory->value() > (int)mSlMemory->maxRAMAlw())
    {
        aWarning = tr (
            "you have assigned more than <b>%1%</b> of your computer's memory "
            "(<b>%2</b>) to the virtual machine. Not enough memory is left "
            "for your host operating system. Please select a smaller amount.")
            .arg ((unsigned)qRound ((double)mSlMemory->maxRAMAlw() / fullSize * 100.0))
            .arg (vboxGlobal().formatSize ((uint64_t)fullSize * _1M));
        return false;
    }
    if (mSlMemory->value() > (int)mSlMemory->maxRAMOpt())
    {
        aWarning = tr (
            "you have assigned more than <b>%1%</b> of your computer's memory "
            "(<b>%2</b>) to the virtual machine. There might not be enough memory "
            "left for your host operating system. Continue at your own risk.")
            .arg ((unsigned)qRound ((double)mSlMemory->maxRAMOpt() / fullSize * 100.0))
            .arg (vboxGlobal().formatSize ((uint64_t)fullSize * _1M));
        return true;
    }

    /* VCPU amount test */
    int totalCPUs = vboxGlobal().virtualBox().GetHost().GetProcessorOnlineCount();
    if (mSlCPU->value() > 2 * totalCPUs)
    {
        aWarning = tr (
            "for performance reasons, the number of virtual CPUs attached to the "
            "virtual machine may not be more than twice the number of physical "
            "CPUs on the host (<b>%1</b>). Please reduce the number of virtual CPUs.")
            .arg (totalCPUs);
        return false;
    }
    if (mSlCPU->value() > totalCPUs)
    {
        aWarning = tr (
            "you have assigned more virtual CPUs to the virtual machine than "
            "the number of physical CPUs on your host system (<b>%1</b>). "
            "This is likely to degrade the performance of your virtual machine. "
            "Please consider reducing the number of virtual CPUs.")
            .arg (totalCPUs);
        return true;
    }

    /* VCPU IO-APIC test */
    if (mSlCPU->value() > 1 && !mCbApic->isChecked())
    {
        aWarning = tr (
            "you have assigned more than one virtual CPU to this VM. "
            "This will not work unless the IO-APIC feature is also enabled. "
            "This will be done automatically when you accept the VM Settings "
            "by pressing the OK button.");
        return true;
    }

    /* VCPU VT-x/AMD-V test */
    if (mSlCPU->value() > 1 && !mCbVirt->isChecked())
    {
        aWarning = tr (
            "you have assigned more than one virtual CPU to this VM. "
            "This will not work unless hardware virtualization (VT-x/AMD-V) is also enabled. "
            "This will be done automatically when you accept the VM Settings "
            "by pressing the OK button.");
        return true;
    }

    /* Chipset type & IO-APIC test */
    if ((KChipsetType)mCbChipset->itemData(mCbChipset->currentIndex()).toInt() == KChipsetType_ICH9 && !mCbApic->isChecked())
    {
        aWarning = tr (
            "you have assigned ICH9 chipset type to this VM. "
            "It will not work properly unless the IO-APIC feature is also enabled. "
            "This will be done automatically when you accept the VM Settings "
            "by pressing the OK button.");
        return true;
    }

    return true;
}

void VBoxVMSettingsSystem::setOrderAfter (QWidget *aWidget)
{
    /* Motherboard tab-order */
    setTabOrder (aWidget, mTwSystem->focusProxy());
    setTabOrder (mTwSystem->focusProxy(), mSlMemory);
    setTabOrder (mSlMemory, mLeMemory);
    setTabOrder (mLeMemory, mTwBootOrder);
    setTabOrder (mTwBootOrder, mTbBootItemUp);
    setTabOrder (mTbBootItemUp, mTbBootItemDown);
    setTabOrder (mTbBootItemDown, mCbApic);
    setTabOrder (mCbApic, mCbEFI);
    setTabOrder (mCbEFI, mCbTCUseUTC);
    setTabOrder (mCbTCUseUTC, mCbUseAbsHID);

    /* Processor tab-order */
    setTabOrder (mCbUseAbsHID, mSlCPU);
    setTabOrder (mSlCPU, mLeCPU);
    setTabOrder (mLeCPU, mCbPae);

    /* Acceleration tab-order */
    setTabOrder (mCbPae, mCbVirt);
    setTabOrder (mCbVirt, mCbNestedPaging);
}

void VBoxVMSettingsSystem::retranslateUi()
{
    /* Translate uic generated strings */
    Ui::VBoxVMSettingsSystem::retranslateUi (this);

    /* Adjust the boot order tree widget */
    mTwBootOrder->header()->setResizeMode (QHeaderView::ResizeToContents);
    mTwBootOrder->resizeColumnToContents (0);
    mTwBootOrder->updateGeometry();
    /* Retranslate the boot order items */
    QTreeWidgetItemIterator it (mTwBootOrder);
    while (*it)
    {
        QTreeWidgetItem *item = (*it);
        item->setText (0, vboxGlobal().toString (
             static_cast <KDeviceType> (item->data (0, ITEM_TYPE_ROLE).toInt())));
        ++ it;
    }
    /* Readjust the tree widget items size */
    adjustBootOrderTWSize();

    CSystemProperties sys = vboxGlobal().virtualBox().GetSystemProperties();

    /* Retranslate the memory slider legend */
    mLbMemoryMin->setText (tr ("<qt>%1&nbsp;MB</qt>").arg (mSlMemory->minRAM()));
    mLbMemoryMax->setText (tr ("<qt>%1&nbsp;MB</qt>").arg (mSlMemory->maxRAM()));

    /* Retranslate the cpu slider legend */
    mLbCPUMin->setText (tr ("<qt>%1&nbsp;CPU</qt>", "%1 is 1 for now").arg (mMinGuestCPU));
    mLbCPUMax->setText (tr ("<qt>%1&nbsp;CPUs</qt>", "%1 is host cpu count * 2 for now").arg (mMaxGuestCPU));
}

void VBoxVMSettingsSystem::valueChangedRAM (int aVal)
{
    mLeMemory->setText (QString().setNum (aVal));
}

void VBoxVMSettingsSystem::textChangedRAM (const QString &aText)
{
    mSlMemory->setValue (aText.toInt());
}

void VBoxVMSettingsSystem::moveBootItemUp()
{
    QTreeWidgetItem *item = mTwBootOrder->currentItem();
    Assert (item);
    if (!mTwBootOrder->itemAbove (item))
        return;

    int index = mTwBootOrder->indexOfTopLevelItem (item);
    QTreeWidgetItem *takenItem = mTwBootOrder->takeTopLevelItem (index);
    Assert (takenItem == item);

    mTwBootOrder->insertTopLevelItem (index - 1, takenItem);
    mTwBootOrder->setCurrentItem (item);

    emit tableChanged();
}

void VBoxVMSettingsSystem::moveBootItemDown()
{
    QTreeWidgetItem *item = mTwBootOrder->currentItem();
    Assert (item);
    if (!mTwBootOrder->itemBelow (item))
        return;

    int index = mTwBootOrder->indexOfTopLevelItem (item);
    QTreeWidgetItem *takenItem = mTwBootOrder->takeTopLevelItem (index);
    Assert (takenItem == item);

    mTwBootOrder->insertTopLevelItem (index + 1, takenItem);
    mTwBootOrder->setCurrentItem (item);

    emit tableChanged();
}

void VBoxVMSettingsSystem::onCurrentBootItemChanged (QTreeWidgetItem *aItem,
                                                     QTreeWidgetItem *)
{
    bool upEnabled   = aItem && mTwBootOrder->itemAbove (aItem);
    bool downEnabled = aItem && mTwBootOrder->itemBelow (aItem);
    if ((mTbBootItemUp->hasFocus() && !upEnabled) ||
        (mTbBootItemDown->hasFocus() && !downEnabled))
        mTwBootOrder->setFocus();
    mTbBootItemUp->setEnabled (upEnabled);
    mTbBootItemDown->setEnabled (downEnabled);
}

void VBoxVMSettingsSystem::adjustBootOrderTWSize()
{
    /* Calculate the optimal size of the tree widget & set it as fixed
     * size. */

    QAbstractItemView *iv = qobject_cast <QAbstractItemView*> (mTwBootOrder);

    int h = 2 * mTwBootOrder->frameWidth();
    int w = h;
#ifdef Q_WS_MAC
    int left, top, right, bottom;
    mTwBootOrder->getContentsMargins (&left, &top, &right, &bottom);
    h += top + bottom;
    w += left + right;
#else /* Q_WS_MAC */
    w += 4;
#endif /* Q_WS_MAC */
    mTwBootOrder->setFixedSize (
        iv->sizeHintForColumn (0) + w,
        iv->sizeHintForRow (0) * mTwBootOrder->topLevelItemCount() + h);

    /* Update the layout system */
    if (mTabMotherboard->layout())
    {
        mTabMotherboard->layout()->activate();
        mTabMotherboard->layout()->update();
    }
}

void VBoxVMSettingsSystem::valueChangedCPU (int aVal)
{
    mLeCPU->setText (QString().setNum (aVal));
}

void VBoxVMSettingsSystem::textChangedCPU (const QString &aText)
{
    mSlCPU->setValue (aText.toInt());
}

bool VBoxVMSettingsSystem::eventFilter (QObject *aObject, QEvent *aEvent)
{
    if (!aObject->isWidgetType())
        return QWidget::eventFilter (aObject, aEvent);

    QWidget *widget = static_cast<QWidget*> (aObject);
    if (widget->window() != window())
        return QWidget::eventFilter (aObject, aEvent);

    switch (aEvent->type())
    {
        case QEvent::FocusIn:
        {
            /* Boot Table */
            if (widget == mTwBootOrder)
            {
                if (!mTwBootOrder->currentItem())
                    mTwBootOrder->setCurrentItem (mTwBootOrder->topLevelItem (0));
                else
                    onCurrentBootItemChanged (mTwBootOrder->currentItem());
                mTwBootOrder->currentItem()->setSelected (true);
            }
            else if (widget != mTbBootItemUp && widget != mTbBootItemDown)
            {
                if (mTwBootOrder->currentItem())
                {
                    mTwBootOrder->currentItem()->setSelected (false);
                    mTbBootItemUp->setEnabled (false);
                    mTbBootItemDown->setEnabled (false);
                }
            }
            break;
        }
        default:
            break;
    }

    return QWidget::eventFilter (aObject, aEvent);
}

