/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_SOLSTICE_LAZY_TOOLS_H
#define KIS_SOLSTICE_LAZY_TOOLS_H

#include <QObject>
#include <QPointer>

class QDialog;
class KisViewManager;
class KisMenuMnemonicFilter;

class KisSolsticeLazyTools : public QObject
{
public:
    explicit KisSolsticeLazyTools(KisViewManager *viewManager);
    ~KisSolsticeLazyTools() override;

    void createActions();

private:
    void createSelectionMask();
    void showSelectionMaskPopup();
    void setForegroundColor(int slot);
    void showFastExportDialog();
    void runSavedExports(bool checkedOnly);
    void showRenameDialog();
    void pickColorFromScreen();
    void applyCustomSettings();

    KisViewManager *m_viewManager{nullptr};
    QPointer<QDialog> m_selectionMaskPopup;
    QPointer<KisMenuMnemonicFilter> m_menuMnemonicFilter;
};

#endif
