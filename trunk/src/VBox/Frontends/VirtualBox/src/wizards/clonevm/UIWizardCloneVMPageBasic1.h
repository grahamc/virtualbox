/* $Id$ */
/** @file
 * VBox Qt GUI - UIWizardCloneVMPageBasic1 class declaration.
 */

/*
 * Copyright (C) 2011-2017 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIWizardCloneVMPageBasic1_h__
#define __UIWizardCloneVMPageBasic1_h__

/* Local includes: */
#include "UIWizardPage.h"

/* Forward declarations: */
class QLineEdit;
class QCheckBox;
class QIRichTextLabel;
class UIVMNamePathSelector;

/* 1st page of the Clone Virtual Machine wizard (base part): */
class UIWizardCloneVMPage1 : public UIWizardPageBase
{
protected:

    UIWizardCloneVMPage1(const QString &strOriginalName, const QString &strDefaultPath, const QString &strGroup);

    QString cloneName() const;
    void    setCloneName(const QString &strName);

    QString clonePath() const;
    void     setClonePath(const QString &strName);

    QString cloneFilePath() const;
    void setCloneFilePath(const QString &path);

    bool isReinitMACsChecked() const;
    /** calls CVirtualBox::ComposeMachineFilename(...) and sets related member variables */
    void composeCloneFilePath();

    QString    m_strOriginalName;
    QString    m_strDefaultPath;
    QString    m_strGroup;
    /** Full, non-native path of the clone machines setting file. Generated by CVirtualBox::ComposeMachineFilename(...) */
    QString    m_strCloneFilePath;
    /** The full path of the folder where clone machine's settings file is located.
     * Generated from the m_strCloneFilePath by removing base file name */
    QString    m_strCloneFolder;
    QCheckBox *m_pReinitMACsCheckBox;
    UIVMNamePathSelector *m_pNamePathSelector;

};

/* 1st page of the Clone Virtual Machine wizard (basic extension): */
class UIWizardCloneVMPageBasic1 : public UIWizardPage, public UIWizardCloneVMPage1
{
    Q_OBJECT;
    Q_PROPERTY(QString cloneName READ cloneName WRITE setCloneName);
    Q_PROPERTY(QString cloneFilePath READ cloneFilePath WRITE setCloneFilePath);
    Q_PROPERTY(bool reinitMACs READ isReinitMACsChecked);

public:

    UIWizardCloneVMPageBasic1(const QString &strOriginalName, const QString &strDefaultPath, const QString &strGroup);

private slots:

    void sltNameChanged();
    void sltPathChanged();

private:

    void retranslateUi();
    void initializePage();

    /* Validation stuff: */
    bool isComplete() const;

    QIRichTextLabel *m_pLabel;
};

#endif // __UIWizardCloneVMPageBasic1_h__