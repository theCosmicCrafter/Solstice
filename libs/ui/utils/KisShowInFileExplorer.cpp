/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisShowInFileExplorer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QUrl>

#include <klocalizedstring.h>

QString KisShowInFileExplorer::actionText()
{
    return i18n("Show in File Explorer");
}

void KisShowInFileExplorer::showFile(const QString &filePath, QWidget *parentWidget)
{
    QString localPath = filePath;
    const QUrl url(filePath);
    if (url.isLocalFile()) {
        localPath = url.toLocalFile();
    }

    const QFileInfo file(localPath);
    const QDir folder = file.absoluteDir();
    if (localPath.isEmpty() || !folder.exists()) {
        QMessageBox::warning(parentWidget,
                             actionText(),
                             i18n("The folder for this file no longer exists:\n%1", filePath));
        return;
    }

#ifdef Q_OS_WIN
    if (file.exists()) {
        QProcess process;
        process.setProgram(QStringLiteral("explorer.exe"));
        // Explorer parses its own command line; /select needs the path quoted
        // directly after the comma, which QProcess argument quoting cannot express.
        process.setNativeArguments(
            QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(file.absoluteFilePath())));
        if (process.startDetached()) {
            return;
        }
    }
#endif

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder.absolutePath()))) {
        QMessageBox::warning(parentWidget, actionText(), i18n("Could not open the folder:\n%1", folder.absolutePath()));
    }
}
