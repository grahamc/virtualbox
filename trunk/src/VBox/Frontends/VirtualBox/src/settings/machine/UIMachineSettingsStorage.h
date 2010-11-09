/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIMachineSettingsStorage class declaration
 */

/*
 * Copyright (C) 2006-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIMachineSettingsStorage_h__
#define __UIMachineSettingsStorage_h__

/* Global includes */
#include <QtGlobal> /* for Q_WS_MAC */
#ifdef Q_WS_MAC
/* Somewhere Carbon.h includes AssertMacros.h which defines the macro "check".
 * In QItemDelegate a class method is called "check" also. As we not used the
 * macro undefine it here. */
# undef check
#endif /* Q_WS_MAC */
#include <QItemDelegate>
#include <QPointer>

/* Local includes */
#include "UISettingsPage.h"
#include "UIMachineSettingsStorage.gen.h"
#include "COMDefs.h"

/* Local forwards */
class AttachmentItem;
class ControllerItem;

/* Internal Types */
typedef QList <StorageSlot> SlotsList;
typedef QList <KDeviceType> DeviceTypeList;
typedef QList <KStorageControllerType> ControllerTypeList;
Q_DECLARE_METATYPE (SlotsList);
Q_DECLARE_METATYPE (DeviceTypeList);
Q_DECLARE_METATYPE (ControllerTypeList);

enum ItemState
{
    State_DefaultItem   = 0,
    State_CollapsedItem = 1,
    State_ExpandedItem  = 2,
    State_MAX
};

/* Pixmap Storage Pool */
class PixmapPool : public QObject
{
    Q_OBJECT;

public:

    enum PixmapType
    {
        InvalidPixmap            = -1,

        ControllerAddEn          =  0,
        ControllerAddDis         =  1,
        ControllerDelEn          =  2,
        ControllerDelDis         =  3,

        AttachmentAddEn          =  4,
        AttachmentAddDis         =  5,
        AttachmentDelEn          =  6,
        AttachmentDelDis         =  7,

        IDEControllerNormal      =  8,
        IDEControllerExpand      =  9,
        IDEControllerCollapse    = 10,
        SATAControllerNormal     = 11,
        SATAControllerExpand     = 12,
        SATAControllerCollapse   = 13,
        SCSIControllerNormal     = 14,
        SCSIControllerExpand     = 15,
        SCSIControllerCollapse   = 16,
        FloppyControllerNormal   = 17,
        FloppyControllerExpand   = 18,
        FloppyControllerCollapse = 19,

        IDEControllerAddEn       = 20,
        IDEControllerAddDis      = 21,
        SATAControllerAddEn      = 22,
        SATAControllerAddDis     = 23,
        SCSIControllerAddEn      = 24,
        SCSIControllerAddDis     = 25,
        FloppyControllerAddEn    = 26,
        FloppyControllerAddDis   = 27,

        HDAttachmentNormal       = 28,
        CDAttachmentNormal       = 29,
        FDAttachmentNormal       = 30,

        HDAttachmentAddEn        = 31,
        HDAttachmentAddDis       = 32,
        CDAttachmentAddEn        = 33,
        CDAttachmentAddDis       = 34,
        FDAttachmentAddEn        = 35,
        FDAttachmentAddDis       = 36,

        VMMEn                    = 37,
        VMMDis                   = 38,

        MaxIndex
    };

    static PixmapPool* pool (QObject *aParent = 0);

    QPixmap pixmap (PixmapType aType) const;

protected:

    PixmapPool (QObject *aParent);

    static QPointer <PixmapPool> mThis;

private:

    QVector <QPixmap> mPool;
};

/* Abstract Controller Type */
class AbstractControllerType
{
public:

    AbstractControllerType (KStorageBus aBusType, KStorageControllerType aCtrType);
    virtual ~AbstractControllerType() {}

    KStorageBus busType() const;
    KStorageControllerType ctrType() const;
    ControllerTypeList ctrTypes() const;
    PixmapPool::PixmapType pixmap (ItemState aState) const;

    void setCtrType (KStorageControllerType aCtrType);

    DeviceTypeList deviceTypeList() const;

protected:

    virtual KStorageControllerType first() const = 0;
    virtual uint size() const = 0;

    KStorageBus mBusType;
    KStorageControllerType mCtrType;
    QList <PixmapPool::PixmapType> mPixmaps;
};

/* IDE Controller Type */
class IDEControllerType : public AbstractControllerType
{
public:

    IDEControllerType (KStorageControllerType aSubType);

private:

    KStorageControllerType first() const;
    uint size() const;
};

/* SATA Controller Type */
class SATAControllerType : public AbstractControllerType
{
public:

    SATAControllerType (KStorageControllerType aSubType);

private:

    KStorageControllerType first() const;
    uint size() const;
};

/* SCSI Controller Type */
class SCSIControllerType : public AbstractControllerType
{
public:

    SCSIControllerType (KStorageControllerType aSubType);

private:

    KStorageControllerType first() const;
    uint size() const;
};

/* Floppy Controller Type */
class FloppyControllerType : public AbstractControllerType
{
public:

    FloppyControllerType (KStorageControllerType aSubType);

private:

    KStorageControllerType first() const;
    uint size() const;
};

/* SAS Controller Type */
class SASControllerType : public AbstractControllerType
{
public:

    SASControllerType (KStorageControllerType aSubType);

private:

    KStorageControllerType first() const;
    uint size() const;
};

/* Abstract Item */
class AbstractItem
{
public:

    enum ItemType
    {
        Type_InvalidItem    = 0,
        Type_RootItem       = 1,
        Type_ControllerItem = 2,
        Type_AttachmentItem = 3
    };

    AbstractItem (AbstractItem *aParent = 0);
    virtual ~AbstractItem();

    AbstractItem* parent() const;
    QUuid id() const;
    QString machineId() const;

    void setMachineId (const QString &aMchineId);

    virtual ItemType rtti() const = 0;
    virtual AbstractItem* childByPos (int aIndex) = 0;
    virtual AbstractItem* childById (const QUuid &aId) = 0;
    virtual int posOfChild (AbstractItem *aItem) const = 0;
    virtual int childCount() const = 0;
    virtual QString text() const = 0;
    virtual QString tip() const = 0;
    virtual QPixmap pixmap (ItemState aState = State_DefaultItem) = 0;

protected:

    virtual void addChild (AbstractItem *aItem) = 0;
    virtual void delChild (AbstractItem *aItem) = 0;

    AbstractItem *mParent;
    QUuid         mId;
    QString       mMachineId;
};
Q_DECLARE_METATYPE (AbstractItem::ItemType);

/* Root Item */
class RootItem : public AbstractItem
{
public:

    RootItem();
   ~RootItem();

    ULONG childCount (KStorageBus aBus) const;

private:

    ItemType rtti() const;
    AbstractItem* childByPos (int aIndex);
    AbstractItem* childById (const QUuid &aId);
    int posOfChild (AbstractItem *aItem) const;
    int childCount() const;
    QString text() const;
    QString tip() const;
    QPixmap pixmap (ItemState aState);
    void addChild (AbstractItem *aItem);
    void delChild (AbstractItem *aItem);

    QList <AbstractItem*> mControllers;
};

/* Controller Item */
class ControllerItem : public AbstractItem
{
public:

    ControllerItem (AbstractItem *aParent, const QString &aName, KStorageBus aBusType,
                    KStorageControllerType aControllerType);
   ~ControllerItem();

    KStorageBus ctrBusType() const;
    QString ctrName() const;
    KStorageControllerType ctrType() const;
    ControllerTypeList ctrTypes() const;
    bool ctrUseIoCache() const;

    void setCtrName (const QString &aCtrName);
    void setCtrType (KStorageControllerType aCtrType);
    void setCtrUseIoCache (bool aUseIoCache);

    SlotsList ctrAllSlots() const;
    SlotsList ctrUsedSlots() const;
    DeviceTypeList ctrDeviceTypeList() const;
    QStringList ctrAllMediumIds (bool aShowDiffs) const;
    QStringList ctrUsedMediumIds() const;

    void setAttachments(const QList<AbstractItem*> &attachments) { mAttachments = attachments; }

private:

    ItemType rtti() const;
    AbstractItem* childByPos (int aIndex);
    AbstractItem* childById (const QUuid &aId);
    int posOfChild (AbstractItem *aItem) const;
    int childCount() const;
    QString text() const;
    QString tip() const;
    QPixmap pixmap (ItemState aState);
    void addChild (AbstractItem *aItem);
    void delChild (AbstractItem *aItem);

    QString mCtrName;
    AbstractControllerType *mCtrType;
    bool mUseIoCache;
    QList <AbstractItem*> mAttachments;
};

/* Attachment Item */
class AttachmentItem : public AbstractItem
{
public:

    AttachmentItem (AbstractItem *aParent, KDeviceType aDeviceType);

    StorageSlot attSlot() const;
    SlotsList attSlots() const;
    KDeviceType attDeviceType() const;
    DeviceTypeList attDeviceTypes() const;
    QString attMediumId() const;
    QStringList attMediumIds (bool aFilter = true) const;
    bool attIsShowDiffs() const;
    bool attIsHostDrive() const;
    bool attIsPassthrough() const;

    void setAttSlot (const StorageSlot &aAttSlot);
    void setAttDevice (KDeviceType aAttDeviceType);
    void setAttMediumId (const QString &aAttMediumId);
    void setAttIsShowDiffs (bool aAttIsShowDiffs);
    void setAttIsPassthrough (bool aPassthrough);

    QString attSize() const;
    QString attLogicalSize() const;
    QString attLocation() const;
    QString attFormat() const;
    QString attUsage() const;

private:

    void cache();

    ItemType rtti() const;
    AbstractItem* childByPos (int aIndex);
    AbstractItem* childById (const QUuid &aId);
    int posOfChild (AbstractItem *aItem) const;
    int childCount() const;
    QString text() const;
    QString tip() const;
    QPixmap pixmap (ItemState aState);
    void addChild (AbstractItem *aItem);
    void delChild (AbstractItem *aItem);

    KDeviceType mAttDeviceType;

    StorageSlot mAttSlot;
    QString mAttMediumId;
    bool mAttIsShowDiffs;
    bool mAttIsHostDrive;
    bool mAttIsPassthrough;

    QString mAttName;
    QString mAttTip;
    QPixmap mAttPixmap;

    QString mAttSize;
    QString mAttLogicalSize;
    QString mAttLocation;
    QString mAttFormat;
    QString mAttUsage;
};

/* Storage Model */
class StorageModel : public QAbstractItemModel
{
    Q_OBJECT;

public:

    enum DataRole
    {
        R_ItemId = Qt::UserRole + 1,
        R_ItemPixmap,
        R_ItemPixmapRect,
        R_ItemName,
        R_ItemNamePoint,
        R_ItemType,
        R_IsController,
        R_IsAttachment,

        R_ToolTipType,
        R_IsMoreIDEControllersPossible,
        R_IsMoreSATAControllersPossible,
        R_IsMoreSCSIControllersPossible,
        R_IsMoreFloppyControllersPossible,
        R_IsMoreSASControllersPossible,
        R_IsMoreAttachmentsPossible,

        R_CtrName,
        R_CtrType,
        R_CtrTypes,
        R_CtrDevices,
        R_CtrBusType,
        R_CtrIoCache,

        R_AttSlot,
        R_AttSlots,
        R_AttDevice,
        R_AttMediumId,
        R_AttIsShowDiffs,
        R_AttIsHostDrive,
        R_AttIsPassthrough,
        R_AttSize,
        R_AttLogicalSize,
        R_AttLocation,
        R_AttFormat,
        R_AttUsage,

        R_Margin,
        R_Spacing,
        R_IconSize,

        R_HDPixmapEn,
        R_CDPixmapEn,
        R_FDPixmapEn,

        R_HDPixmapAddEn,
        R_HDPixmapAddDis,
        R_CDPixmapAddEn,
        R_CDPixmapAddDis,
        R_FDPixmapAddEn,
        R_FDPixmapAddDis,
        R_HDPixmapRect,
        R_CDPixmapRect,
        R_FDPixmapRect
    };

    enum ToolTipType
    {
        DefaultToolTip  = 0,
        ExpanderToolTip = 1,
        HDAdderToolTip  = 2,
        CDAdderToolTip  = 3,
        FDAdderToolTip  = 4
    };

    StorageModel (QObject *aParent);
   ~StorageModel();

    int rowCount (const QModelIndex &aParent = QModelIndex()) const;
    int columnCount (const QModelIndex &aParent = QModelIndex()) const;

    QModelIndex root() const;
    QModelIndex index (int aRow, int aColumn, const QModelIndex &aParent = QModelIndex()) const;
    QModelIndex parent (const QModelIndex &aIndex) const;

    QVariant data (const QModelIndex &aIndex, int aRole) const;
    bool setData (const QModelIndex &aIndex, const QVariant &aValue, int aRole);

    QModelIndex addController (const QString &aCtrName, KStorageBus aBusType, KStorageControllerType aCtrType);
    void delController (const QUuid &aCtrId);

    QModelIndex addAttachment (const QUuid &aCtrId, KDeviceType aDeviceType);
    void delAttachment (const QUuid &aCtrId, const QUuid &aAttId);

    void setMachineId (const QString &aMachineId);

    void sort(int iColumn = 0, Qt::SortOrder order = Qt::AscendingOrder);
    QModelIndex attachmentBySlot(QModelIndex controllerIndex, StorageSlot attachmentStorageSlot);

private:

    Qt::ItemFlags flags (const QModelIndex &aIndex) const;

    AbstractItem *mRootItem;

    QPixmap mPlusPixmapEn;
    QPixmap mPlusPixmapDis;

    QPixmap mMinusPixmapEn;
    QPixmap mMinusPixmapDis;

    ToolTipType mToolTipType;

    KChipsetType getChipsetType() const;
};
Q_DECLARE_METATYPE (StorageModel::ToolTipType);

/* Storage Delegate */
class StorageDelegate : public QItemDelegate
{
    Q_OBJECT;

public:

    StorageDelegate (QObject *aParent);

private:

    void paint (QPainter *aPainter, const QStyleOptionViewItem &aOption, const QModelIndex &aIndex) const;

    bool mDisableStaticControls;
};

/* Machine settings / Storage page / Attachment data: */
struct UIStorageAttachmentData
{
    KDeviceType m_attachmentType;
    LONG m_iAttachmentPort;
    LONG m_iAttachmentDevice;
    QString m_strAttachmentMediumId;
    bool m_fAttachmentPassthrough;
};

/* Machine settings / Storage page / Controller data: */
struct UIStorageControllerData
{
    QString m_strControllerName;
    KStorageBus m_controllerBus;
    KStorageControllerType m_controllerType;
    bool m_fUseHostIOCache;
    QList<UIStorageAttachmentData> m_items;
};

/* Machine settings / Storage page / Cache: */
struct UISettingsCacheMachineStorage
{
    QString m_strMachineId;
    QList<UIStorageControllerData> m_items;
};

/* Machine settings / Storage page: */
class UIMachineSettingsStorage : public UISettingsPageMachine,
                         public Ui::UIMachineSettingsStorage
{
    Q_OBJECT;

public:

    UIMachineSettingsStorage();

signals:

    void storageChanged();

protected:

    /* Load data to cashe from corresponding external object(s),
     * this task COULD be performed in other than GUI thread: */
    void loadToCacheFrom(QVariant &data);
    /* Load data to corresponding widgets from cache,
     * this task SHOULD be performed in GUI thread only: */
    void getFromCache();

    /* Save data from corresponding widgets to cache,
     * this task SHOULD be performed in GUI thread only: */
    void putToCache();
    /* Save data from cache to corresponding external object(s),
     * this task COULD be performed in other than GUI thread: */
    void saveFromCacheTo(QVariant &data);

    void setValidator (QIWidgetValidator *aVal);
    bool revalidate (QString &aWarning, QString &aTitle);

    void retranslateUi();

    void showEvent (QShowEvent *aEvent);

private slots:

    void mediumUpdated (const VBoxMedium &aMedium);
    void mediumRemoved (VBoxDefs::MediumType aType, const QString &aMediumId);

    void addController();
    void addIDEController();
    void addSATAController();
    void addSCSIController();
    void addFloppyController();
    void addSASController();
    void delController();

    void addAttachment();
    void addHDAttachment();
    void addCDAttachment();
    void addFDAttachment();
    void delAttachment();

    void getInformation();
    void setInformation();

    void sltOpenMedium();
    void sltNewMedium();

    void updateActionsState();

    void onRowInserted (const QModelIndex &aParent, int aIndex);
    void onRowRemoved();

    void onCurrentItemChanged();

    void onContextMenuRequested (const QPoint &aPosition);

    void onDrawItemBranches (QPainter *aPainter, const QRect &aRect, const QModelIndex &aIndex);

    void onMouseMoved (QMouseEvent *aEvent);
    void onMouseClicked (QMouseEvent *aEvent);

private:

    void addControllerWrapper (const QString &aName, KStorageBus aBus, KStorageControllerType aType);
    void addAttachmentWrapper (KDeviceType aDevice);

    QString getWithNewHDWizard();

    void updateAdditionalObjects (KDeviceType aType);

    QString generateUniqueName (const QString &aTemplate) const;

    uint32_t deviceCount (KDeviceType aType) const;

    QIWidgetValidator *mValidator;

    StorageModel *mStorageModel;

    QAction *mAddCtrAction;
    QAction *mDelCtrAction;
    QAction *mAddIDECtrAction;
    QAction *mAddSATACtrAction;
    QAction *mAddSCSICtrAction;
    QAction *mAddSASCtrAction;
    QAction *mAddFloppyCtrAction;
    QAction *mAddAttAction;
    QAction *mDelAttAction;
    QAction *mAddHDAttAction;
    QAction *mAddCDAttAction;
    QAction *mAddFDAttAction;

    bool mIsLoadingInProgress;
    bool mIsPolished;
    bool mDisableStaticControls;

    /* Cache: */
    UISettingsCacheMachineStorage m_cache;
};

#endif // __UIMachineSettingsStorage_h__

