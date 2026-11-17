/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisSolsticeProfile.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStorageInfo>

#include <KConfig>
#include <KConfigGroup>

#include "KisSolsticePaths.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
const char *const MarkerFile = "SOLSTICE_PROFILE";
const char *const ResourceDirectoryKey = "ResourceDirectory";

const QStringList &configurationFiles()
{
    static const QStringList files = {
        QStringLiteral("kritarc"),
        QStringLiteral("kritadisplayrc"),
        QStringLiteral("kritashortcutsrc"),
        QStringLiteral("krita-scripterrc"),
        QStringLiteral("karboncalligraphyrc"),
        QStringLiteral("klanguageoverridesrc"),
    };
    return files;
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

// The errors are shown in krita/main.cc's built-in messages, partly before Qt
// starts, so they are built in too (KisSolsticeProfile::builtInText()).

QString couldNotCreate(const QString &path)
{
    return KisSolsticeProfile::builtInText(QStringLiteral("Could not create %1"),
                                           QStringLiteral("%1 を作成できませんでした"),
                                           QStringLiteral("无法创建 %1"))
        .arg(QDir::toNativeSeparators(path));
}

QString couldNotCopy(const QString &path)
{
    return KisSolsticeProfile::builtInText(QStringLiteral("Could not copy %1"),
                                           QStringLiteral("%1 をコピーできませんでした"),
                                           QStringLiteral("无法复制 %1"))
        .arg(QDir::toNativeSeparators(path));
}

QString couldNotWriteMarker()
{
    return KisSolsticeProfile::builtInText(QStringLiteral("Could not write the profile marker"),
                                           QStringLiteral("プロファイルのマーカーファイルを書き込めませんでした"),
                                           QStringLiteral("无法写入用户配置的标记文件"));
}

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive)
        == 0;
}

bool makeFolders(QString *error)
{
    for (const QString &dir : {KisSolsticePaths::configDir(),
                               KisSolsticePaths::logDir(),
                               KisSolsticePaths::defaultResourceDir(),
                               KisSolsticePaths::cacheDir()}) {
        if (!QDir().mkpath(dir)) {
            setError(error, couldNotCreate(dir));
            return false;
        }
    }
    return true;
}

bool copyTree(const QString &source, const QString &target, QString *error)
{
    QDirIterator it(source, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString destination = QDir(target).filePath(QDir(source).relativeFilePath(file));
        QDir().mkpath(QFileInfo(destination).path());
        QFile::remove(destination);
        if (!QFile::copy(file, destination)) {
            setError(error, couldNotCopy(file));
            return false;
        }
    }
    return true;
}

/// Copies @p source to @p destination in chunks, keeping the modification
/// time (the resource database compares timestamps).
bool copyFile(const QString &source,
              const QString &destination,
              qint64 *done,
              const std::function<bool(qint64)> &progress,
              bool *cancelled)
{
    QFile in(source);
    QFile out(destination);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QByteArray buffer;
    while (!in.atEnd()) {
        buffer = in.read(4 * 1024 * 1024);
        if (buffer.isEmpty() && in.error() != QFileDevice::NoError) {
            return false;
        }
        if (out.write(buffer) != buffer.size()) {
            return false;
        }
        *done += buffer.size();
        if (progress && !progress(*done)) {
            *cancelled = true;
            return false;
        }
    }
    out.close();
    in.close();
    out.open(QIODevice::Append);
    out.setFileTime(QFileInfo(source).lastModified(), QFileDevice::FileModificationTime);
    return true;
}
} // namespace

KisSolsticeProfile::State KisSolsticeProfile::state()
{
    QFile marker(KisSolsticePaths::configFilePath(QString::fromLatin1(MarkerFile)));
    if (!marker.open(QIODevice::ReadOnly)) {
        return State::Missing;
    }
    const QByteArray value = marker.readAll().trimmed();
    if (value == "ready") {
        return State::Ready;
    }
    if (value == "importing") {
        return State::Importing;
    }
    return State::Missing;
}

bool KisSolsticeProfile::setState(State state)
{
    const QString path = KisSolsticePaths::configFilePath(QString::fromLatin1(MarkerFile));
    if (state == State::Missing) {
        return !QFile::exists(path) || QFile::remove(path);
    }
    QDir().mkpath(KisSolsticePaths::configDir());
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    marker.write(state == State::Ready ? "ready\n" : "importing\n");
    return marker.flush();
}

bool KisSolsticeProfile::legacyProfileExists()
{
    return QFile::exists(QDir(KisSolsticePaths::legacyConfigDir()).filePath(QStringLiteral("kritarc")))
        || QFile::exists(QDir(KisSolsticePaths::legacyResourceDir()).filePath(QStringLiteral("resourcecache.sqlite")));
}

QString KisSolsticeProfile::legacyLanguage()
{
    QSettings overrides(QDir(KisSolsticePaths::legacyConfigDir()).filePath(QStringLiteral("klanguageoverridesrc")),
                        QSettings::IniFormat);
    overrides.beginGroup(QStringLiteral("Language"));
    const QString languages = overrides.value(QStringLiteral("krita")).toString();
    return languages.section(QLatin1Char(':'), 0, 0);
}

KisSolsticeProfile::TextLanguage KisSolsticeProfile::textLanguageFor(const QString &legacyLanguage,
                                                                     quint16 windowsLanguageId)
{
    if (legacyLanguage.startsWith(QLatin1String("ja"))) {
        return TextLanguage::Japanese;
    }
    if (legacyLanguage == QLatin1String("zh_CN") || legacyLanguage == QLatin1String("zh_SG")) {
        return TextLanguage::ChineseSimplified;
    }
    // LANGID: primary language in the low 10 bits, sublanguage above
    const quint16 primary = windowsLanguageId & 0x3ff;
    const quint16 sub = windowsLanguageId >> 10;
    if (primary == 0x11) { // LANG_JAPANESE
        return TextLanguage::Japanese;
    }
    // LANG_CHINESE with SUBLANG_CHINESE_SIMPLIFIED or SUBLANG_CHINESE_SINGAPORE
    if (primary == 0x04 && (sub == 0x02 || sub == 0x04)) {
        return TextLanguage::ChineseSimplified;
    }
    return TextLanguage::English;
}

KisSolsticeProfile::TextLanguage KisSolsticeProfile::textLanguage()
{
#ifdef Q_OS_WIN
    return textLanguageFor(legacyLanguage(), GetUserDefaultUILanguage());
#else
    return textLanguageFor(legacyLanguage(), 0);
#endif
}

QString KisSolsticeProfile::builtInText(const QString &english, const QString &japanese, const QString &chinese)
{
    switch (textLanguage()) {
    case TextLanguage::Japanese:
        return japanese;
    case TextLanguage::ChineseSimplified:
        return chinese;
    case TextLanguage::English:
        break;
    }
    return english;
}

bool KisSolsticeProfile::createFreshProfile(QString *error)
{
    if (!makeFolders(error)) {
        return false;
    }
    // Solstice defaults (docs/agent/settings-location.md): a new
    // profile starts in English. The format is the one of
    // kswitchlanguagedialog_p.cpp: [Language] <application name>=<languages>.
    {
        QSettings overrides(KisSolsticePaths::configFilePath(QStringLiteral("klanguageoverridesrc")),
                            QSettings::IniFormat);
        overrides.beginGroup(QStringLiteral("Language"));
        if (!overrides.contains(QStringLiteral("krita"))) {
            overrides.setValue(QStringLiteral("krita"), QByteArray("en_US"));
        }
        overrides.endGroup();
        overrides.sync();
    }
    if (!setState(State::Ready)) {
        setError(error, couldNotWriteMarker());
        return false;
    }
    return true;
}

QString KisSolsticeProfile::rewritePaths(const QString &text,
                                         const QString &oldDir,
                                         const QString &newDir,
                                         const QString &homeDir)
{
    const QString oldForward = QDir::cleanPath(QDir::fromNativeSeparators(oldDir));
    const QString newForward = QDir::cleanPath(QDir::fromNativeSeparators(newDir));
    const QString home = QDir::cleanPath(QDir::fromNativeSeparators(homeDir));

    QList<QPair<QString, QString>> forms;
    auto backslashes = [](QString path) {
        return path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    };
    auto escaped = [&](const QString &path) {
        return backslashes(path).replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    };
    forms.append({escaped(oldForward), escaped(newForward)});
    forms.append({backslashes(oldForward), backslashes(newForward)});
    forms.append({oldForward, newForward});
    if (!home.isEmpty() && oldForward.startsWith(home + QLatin1Char('/'), Qt::CaseInsensitive)) {
        const QString newHomeForm = newForward.startsWith(home + QLatin1Char('/'), Qt::CaseInsensitive)
            ? QStringLiteral("$HOME") + newForward.mid(home.size())
            : newForward;
        forms.append({QStringLiteral("$HOME") + oldForward.mid(home.size()), newHomeForm});
    }

    QString result = text;
    for (const auto &form : forms) {
        // Whole folder names only: "krita" must not match "krita - Copy", so
        // spaces do not end a name; separators, quotes and line ends do.
        const QRegularExpression pattern(
            QRegularExpression::escape(form.first) + QStringLiteral("(?=$|[/\\\\\"',;\\]\\}\\r\\n])"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        // Literal replacement (QString::replace() would read "\\1" in it).
        QString replaced;
        qsizetype last = 0;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(result);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            replaced += QStringView(result).mid(last, match.capturedStart() - last);
            replaced += form.second;
            last = match.capturedEnd();
        }
        replaced += QStringView(result).mid(last);
        result = replaced;
    }
    return result;
}

bool KisSolsticeProfile::resourcesToImport()
{
    const QString legacyResources = KisSolsticePaths::legacyResourceDir();
    if (!QFileInfo(legacyResources).isDir()) {
        return false;
    }
    const QString legacyKritarc = QDir(KisSolsticePaths::legacyConfigDir()).filePath(QStringLiteral("kritarc"));
    if (!QFile::exists(legacyKritarc)) {
        return true;
    }
    KConfig config(legacyKritarc, KConfig::SimpleConfig);
    const QString custom = KConfigGroup(&config, QString()).readEntry(ResourceDirectoryKey, QString());
    return custom.isEmpty() || samePath(custom, legacyResources);
}

bool KisSolsticeProfile::importConfiguration(QString *error)
{
    if (!makeFolders(error)) {
        return false;
    }
    const bool withResources = resourcesToImport();
    const QDir legacy(KisSolsticePaths::legacyConfigDir());
    for (const QString &name : configurationFiles()) {
        const QString source = legacy.filePath(name);
        if (!QFile::exists(source)) {
            continue;
        }
        const QString target = KisSolsticePaths::configFilePath(name);
        QFile::remove(target);
        if (name == QLatin1String("kritarc") && withResources) {
            QFile in(source);
            QFile out(target);
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
                setError(error, couldNotCopy(source));
                return false;
            }
            const QString text = QString::fromUtf8(in.readAll());
            out.write(rewritePaths(text,
                                   KisSolsticePaths::legacyResourceDir(),
                                   KisSolsticePaths::defaultResourceDir(),
                                   QDir::homePath())
                          .toUtf8());
        } else if (!QFile::copy(source, target)) {
            setError(error, couldNotCopy(source));
            return false;
        }
    }
    // KXmlGui's local files lived in the Krita resource folder (AppDataLocation).
    const QString xmlgui = QDir(KisSolsticePaths::legacyResourceDir()).filePath(QStringLiteral("kxmlgui5"));
    if (QFileInfo(xmlgui).isDir()
        && !copyTree(xmlgui, QDir(KisSolsticePaths::xmlguiDataDir()).filePath(QStringLiteral("kxmlgui5")), error)) {
        return false;
    }
    if (!setState(withResources ? State::Importing : State::Ready)) {
        setError(error, couldNotWriteMarker());
        return false;
    }
    return true;
}

KisSolsticeProfile::ResourceCopyPlan KisSolsticeProfile::planResourceCopy()
{
    ResourceCopyPlan plan;
    plan.sourceDir = KisSolsticePaths::legacyResourceDir();
    plan.targetDir = KisSolsticePaths::defaultResourceDir();
    const QDir source(plan.sourceDir);
    const QRegularExpression databaseBackup(QStringLiteral("^resourcecache\\.sqlite\\.\\d+~$"));
    QDirIterator it(plan.sourceDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = source.relativeFilePath(file);
        if (databaseBackup.match(relative).hasMatch() || relative.startsWith(QLatin1String("kxmlgui5/"))) {
            continue;
        }
        plan.files.append(relative);
        plan.bytes += it.fileInfo().size();
    }
    return plan;
}

bool KisSolsticeProfile::copyResources(const ResourceCopyPlan &plan,
                                       const std::function<bool(qint64)> &progress,
                                       QString *error)
{
    QDir target(plan.targetDir);
    if (target.exists() && !target.removeRecursively()) {
        setError(error,
                 builtInText(QStringLiteral("Could not clear %1"),
                             QStringLiteral("%1 を空にできませんでした"),
                             QStringLiteral("无法清空 %1"))
                     .arg(QDir::toNativeSeparators(plan.targetDir)));
        return false;
    }
    QDir().mkpath(plan.targetDir);
    const QStorageInfo storage(plan.targetDir);
    const qint64 margin = 64 * 1024 * 1024;
    if (storage.isValid() && storage.bytesAvailable() < plan.bytes + margin) {
        setError(error,
                 builtInText(QStringLiteral("Not enough free space: %1 MB needed, %2 MB available"),
                             QStringLiteral("空き容量が足りません: %1 MB 必要ですが、%2 MB しかありません"),
                             QStringLiteral("可用空间不足：需要 %1 MB，可用 %2 MB"))
                     .arg((plan.bytes + margin) / (1024 * 1024))
                     .arg(storage.bytesAvailable() / (1024 * 1024)));
        target.removeRecursively();
        return false;
    }

    const QDir source(plan.sourceDir);
    qint64 done = 0;
    bool cancelled = false;
    for (const QString &relative : plan.files) {
        const QString destination = target.filePath(relative);
        QDir().mkpath(QFileInfo(destination).path());
        if (!copyFile(source.filePath(relative), destination, &done, progress, &cancelled)) {
            setError(error,
                     cancelled ? builtInText(QStringLiteral("Cancelled"),
                                             QStringLiteral("キャンセルされました"),
                                             QStringLiteral("已取消"))
                               : couldNotCopy(relative));
            target.removeRecursively();
            return false;
        }
    }
    if (!setState(State::Ready)) {
        setError(error, couldNotWriteMarker());
        return false;
    }
    return true;
}

void KisSolsticeProfile::abandonImport()
{
    QDir(KisSolsticePaths::defaultResourceDir()).removeRecursively();
    QDir(KisSolsticePaths::configDir()).removeRecursively();
}
