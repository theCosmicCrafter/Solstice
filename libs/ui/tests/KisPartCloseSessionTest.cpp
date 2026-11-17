/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QDialog>
#include <QScopedPointer>
#include <QTimer>

#include "KisPart.h"
#include <testui.h>

class KisPartCloseSessionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testRefusedUnderModalDialog_data();
    void testRefusedUnderModalDialog();
};

void KisPartCloseSessionTest::testRefusedUnderModalDialog_data()
{
    QTest::addColumn<bool>("windowModal");
    QTest::newRow("application-modal") << false;
    QTest::newRow("window-modal") << true;
}

// File > Quit (or a script) while a dialog of a main window runs in exec()
// used to delete the window, and with it the dialog on the stack of exec():
// heap corruption on Windows. A test cannot create a KisMainWindow (it needs
// a KisApplication), so this checks the refusal itself.
void KisPartCloseSessionTest::testRefusedUnderModalDialog()
{
    QFETCH(bool, windowModal);
    QScopedPointer<QWidget> window;
    if (windowModal) {
        window.reset(new QWidget());
        window->show();
    }
    QDialog dialog(window.data());
    if (windowModal) {
        dialog.setWindowModality(Qt::WindowModal);
    }

    bool closed = true;
    bool keptWindows = false;
    QTimer::singleShot(0, &dialog, [&] {
        closed = KisPart::instance()->closeSession();
        // switching sessions keeps the windows and is still allowed
        keptWindows = KisPart::instance()->closeSession(true);
        dialog.reject();
    });
    // never hang the test run
    QTimer::singleShot(10000, &dialog, &QDialog::reject);
    dialog.exec();

    QVERIFY(!closed);
    QVERIFY(keptWindows);
    QVERIFY(!KisPart::instance()->closingSession());
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(KisPart::instance()->closeSession());
}

KISTEST_MAIN(KisPartCloseSessionTest)
#include "KisPartCloseSessionTest.moc"
