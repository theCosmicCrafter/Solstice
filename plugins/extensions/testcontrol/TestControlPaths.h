/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTCONTROLPATHS_H
#define TESTCONTROLPATHS_H

#include <QString>

/**
 * Confines the document tools' files to the test root given at launch
 * (SOLSTICE_TEST_CONTROL_ROOT). Depends on QtCore only.
 */
namespace TestControlPaths
{
/// The canonical test root for @p value (the environment variable), or an
/// empty string when it is unset, relative or not an existing directory.
QString testRoot(const QString &value);

struct Resolved {
    /// Absolute path inside the root; empty on error.
    QString path;
    QString error;
};

/**
 * Resolves @p relativePath against @p root. The path must be relative; after
 * resolving `..` and symbolic links it must stay inside the root. With
 * @p mustExist the file must exist; otherwise its directory must exist and the
 * path must not name a directory.
 */
Resolved resolve(const QString &root, const QString &relativePath, bool mustExist);
} // namespace TestControlPaths

#endif // TESTCONTROLPATHS_H
