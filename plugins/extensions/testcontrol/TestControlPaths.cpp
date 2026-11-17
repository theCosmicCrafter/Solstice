/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlPaths.h"

#include <QDir>
#include <QFileInfo>

namespace
{
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity PathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity PathCase = Qt::CaseSensitive;
#endif

bool isInside(const QString &root, const QString &canonicalPath)
{
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return canonicalPath.startsWith(prefix, PathCase);
}

TestControlPaths::Resolved failure(const QString &error)
{
    return {QString(), error};
}
} // namespace

namespace TestControlPaths
{
QString testRoot(const QString &value)
{
    if (value.isEmpty() || QDir::isRelativePath(value)) {
        return QString();
    }
    const QFileInfo info(value);
    if (!info.isDir()) {
        return QString();
    }
    return info.canonicalFilePath();
}

Resolved resolve(const QString &root, const QString &relativePath, bool mustExist)
{
    if (root.isEmpty()) {
        return failure(
            QStringLiteral("No test root; start Solstice with SOLSTICE_TEST_CONTROL_ROOT set to an "
                           "existing directory"));
    }
    if (relativePath.isEmpty()) {
        return failure(QStringLiteral("The path is empty"));
    }
    if (!QDir::isRelativePath(relativePath)) {
        return failure(QStringLiteral("The path must be relative to the test root: ") + relativePath);
    }

    const QString joined = QDir::cleanPath(root + QLatin1Char('/') + relativePath);
    const QFileInfo info(joined);

    if (mustExist) {
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !info.isFile()) {
            return failure(QStringLiteral("No such file in the test root: ") + relativePath);
        }
        if (!isInside(root, canonical)) {
            return failure(QStringLiteral("The path leaves the test root: ") + relativePath);
        }
        return {canonical, QString()};
    }

    if (info.isDir()) {
        return failure(QStringLiteral("The path names a directory: ") + relativePath);
    }
    const QString directory = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (directory.isEmpty()) {
        return failure(QStringLiteral("The directory does not exist in the test root: ") + relativePath);
    }
    if (directory.compare(root, PathCase) != 0 && !isInside(root, directory)) {
        return failure(QStringLiteral("The path leaves the test root: ") + relativePath);
    }
    if (info.exists() && !isInside(root, info.canonicalFilePath())) {
        // An existing symbolic link that points out of the root.
        return failure(QStringLiteral("The path leaves the test root: ") + relativePath);
    }
    return {directory + QLatin1Char('/') + info.fileName(), QString()};
}
} // namespace TestControlPaths
