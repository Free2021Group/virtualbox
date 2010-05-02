/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineWindowSeamless class declaration
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIMachineWindowSeamless_h__
#define __UIMachineWindowSeamless_h__

/* Global includes */
#include <QMainWindow>

/* Local includes */
#include "QIWithRetranslateUI.h"
#include "UIMachineWindow.h"
#ifdef Q_WS_X11
# include <X11/Xlib.h>
#endif

/* Local forwards */
class VBoxMiniToolBar;

class UIMachineWindowSeamless : public QIWithRetranslateUI2<QMainWindow>, public UIMachineWindow
{
    Q_OBJECT;

public slots:

    void sltPlaceOnScreen();

protected:

    /* Seamless machine window constructor/destructor: */
    UIMachineWindowSeamless(UIMachineLogic *pMachineLogic, ulong uScreenId);
    virtual ~UIMachineWindowSeamless();

private slots:

    /* Console callback handlers: */
    void sltMachineStateChanged();

    /* Popup main menu: */
    void sltPopupMainMenu();

    /* Update mini tool-bar mask: */
    void sltUpdateMiniToolBarMask();

    /* Close window reimplementation: */
    void sltTryClose();

private:

    /* Translate routine: */
    void retranslateUi();

    /* Event handlers: */
#ifdef Q_WS_MAC
    bool event(QEvent *pEvent);
#endif /* Q_WS_MAC */
#ifdef Q_WS_X11
    bool x11Event(XEvent *pEvent);
#endif /* Q_WS_X11 */
    void closeEvent(QCloseEvent *pEvent);

    /* Prepare helpers: */
    void prepareSeamless();
    void prepareMenu();
    void prepareMiniToolBar();
    void prepareMachineView();
#ifdef Q_WS_MAC
    void loadWindowSettings();
#endif /* Q_WS_MAC */

    /* Cleanup helpers: */
    void saveWindowSettings();
    void cleanupMachineView();
    //void cleanupMiniToolBar() {}
    void cleanupMenu();
    //void cleanupSeamless() {}

    /* Update routines: */
    void updateAppearanceOf(int iElement);

    /* Other members: */
    void showSeamless();
    void setMask(const QRegion &region);

    /* Private variables: */
    QMenu *m_pMainMenu;
    VBoxMiniToolBar *m_pMiniToolBar;
#ifdef Q_WS_WIN
    QRegion m_prevRegion;
#endif /* Q_WS_WIN */

    /* Factory support: */
    friend class UIMachineWindow;
};

#endif // __UIMachineWindowSeamless_h__
