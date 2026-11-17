/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <simpletest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "KisSolsticePaths.h"
#include "KisSolsticeProfile.h"

namespace
{
void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// Relative path and hash of every file under @p dir.
QMap<QString, QByteArray> snapshot(const QString &dir)
{
    QMap<QString, QByteArray> result;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        result.insert(QDir(dir).relativeFilePath(file),
                      QCryptographicHash::hash(readFile(file), QCryptographicHash::Sha1)
                          + it.fileInfo().lastModified().toString(Qt::ISODateWithMs).toUtf8());
    }
    return result;
}
} // namespace

class KisSolsticeProfileTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void cleanup();
    void testRewritePaths();
    void testFreshProfile();
    void testImport();
    void testCustomResourceDirectoryIsKept();
    void testCancelledCopy();
    void testTextLanguage();

private:
    void createKritaProfile(const QByteArray &resourceDirectoryLine);

    QScopedPointer<QTemporaryDir> m_dir;
    QString m_legacyConfig;
    QString m_legacyResources;
};

void KisSolsticeProfileTest::init()
{
    m_dir.reset(new QTemporaryDir);
    QVERIFY(m_dir->isValid());
    m_legacyConfig = m_dir->filePath("Local");
    m_legacyResources = m_dir->filePath("Roaming/krita");
    QDir().mkpath(m_legacyConfig);
    qputenv("SOLSTICE_PROFILE_ROOT", QFile::encodeName(QDir::toNativeSeparators(m_dir->filePath("Roaming/Solstice"))));
    qputenv("SOLSTICE_LEGACY_CONFIG_DIR", QFile::encodeName(QDir::toNativeSeparators(m_legacyConfig)));
    qputenv("SOLSTICE_LEGACY_RESOURCE_DIR", QFile::encodeName(QDir::toNativeSeparators(m_legacyResources)));
}

void KisSolsticeProfileTest::cleanup()
{
    qunsetenv("SOLSTICE_PROFILE_ROOT");
    qunsetenv("SOLSTICE_LEGACY_CONFIG_DIR");
    qunsetenv("SOLSTICE_LEGACY_RESOURCE_DIR");
    m_dir.reset();
}

void KisSolsticeProfileTest::createKritaProfile(const QByteArray &resourceDirectoryLine)
{
    const QByteArray forward = QDir::cleanPath(m_legacyResources).toUtf8();
    QByteArray escaped = QDir::toNativeSeparators(QDir::cleanPath(m_legacyResources)).toUtf8();
    escaped.replace("\\", "\\\\");
    writeFile(m_legacyConfig + "/kritarc",
              resourceDirectoryLine + "\n" + "AlwaysUseTemplate=" + forward + "/templates/a.kra\n" + "OtherCopy="
                  + forward + " - Copy/x\n" + "[pigment_o]\nmask_set=" + escaped + "\\\\pykrita\\\\MASK\n");
    writeFile(m_legacyConfig + "/kritadisplayrc", "[General]\nSolsticeInterfaceScale=125\n");
    writeFile(m_legacyConfig + "/klanguageoverridesrc", "[Language]\nkrita=@ByteArray(ja:en_US)\n");
    writeFile(m_legacyResources + "/resourcecache.sqlite", QByteArray(100000, 'd'));
    writeFile(m_legacyResources + "/resourcecache.sqlite.1~", "backup");
    writeFile(m_legacyResources + "/KRITA_RESOURCE_VERSION", "6.0.5-prealpha");
    writeFile(m_legacyResources + "/brushes/a.gbr", "brush");
    writeFile(m_legacyResources + "/quickaccess/default.kqap", "profile");
    writeFile(m_legacyResources + "/kxmlgui5/krita/custom.xmlgui", "<gui/>");
}

void KisSolsticeProfileTest::testRewritePaths()
{
    const QString oldDir = "C:/Users/u/AppData/Roaming/krita";
    const QString newDir = "C:/Users/u/AppData/Roaming/Solstice/resources";
    const QString home = "C:/Users/u";
    const QString text =
        "ResourceDirectory=C:/Users/u/AppData/Roaming/krita\n"
        "a=c:/users/u/appdata/roaming/KRITA/templates/x.kra\n"
        "b=C:\\\\Users\\\\u\\\\AppData\\\\Roaming\\\\krita\\\\pykrita\n"
        "c={\"watch\": \"C:/Users/u/AppData/Roaming/krita/pykrita/p.py\"}\n"
        "d[$e]=$HOME/AppData/Roaming/krita/templates/y.kra\n"
        "e=C:/Users/u/AppData/Roaming/krita - Copy/z\n"
        "f=C:/Users/u/AppData/Roaming/kritaX\n"
        "g=C:/Users/u/AppData/Local/Temp\n";
    const QString expected =
        "ResourceDirectory=C:/Users/u/AppData/Roaming/Solstice/resources\n"
        "a=C:/Users/u/AppData/Roaming/Solstice/resources/templates/x.kra\n"
        "b=C:\\\\Users\\\\u\\\\AppData\\\\Roaming\\\\Solstice\\\\resources\\\\pykrita\n"
        "c={\"watch\": \"C:/Users/u/AppData/Roaming/Solstice/resources/pykrita/p.py\"}\n"
        "d[$e]=$HOME/AppData/Roaming/Solstice/resources/templates/y.kra\n"
        "e=C:/Users/u/AppData/Roaming/krita - Copy/z\n"
        "f=C:/Users/u/AppData/Roaming/kritaX\n"
        "g=C:/Users/u/AppData/Local/Temp\n";
    QCOMPARE(KisSolsticeProfile::rewritePaths(text, oldDir, newDir, home), expected);
}

void KisSolsticeProfileTest::testFreshProfile()
{
    using namespace KisSolsticeProfile;
    QCOMPARE(state(), State::Missing);
    QVERIFY(!legacyProfileExists());
    QVERIFY(createFreshProfile());
    QCOMPARE(state(), State::Ready);
    // A new profile starts in English.
    QSettings overrides(KisSolsticePaths::configFilePath("klanguageoverridesrc"), QSettings::IniFormat);
    QCOMPARE(overrides.value("Language/krita").toByteArray(), QByteArray("en_US"));
    for (const QString &dir : {KisSolsticePaths::configDir(),
                               KisSolsticePaths::logDir(),
                               KisSolsticePaths::defaultResourceDir(),
                               KisSolsticePaths::cacheDir()}) {
        QVERIFY2(QFileInfo(dir).isDir(), qPrintable(dir));
    }
}

void KisSolsticeProfileTest::testImport()
{
    using namespace KisSolsticeProfile;
    createKritaProfile("ResourceDirectory=" + QDir::cleanPath(m_legacyResources).toUtf8());
    const auto legacyConfigBefore = snapshot(m_legacyConfig);
    const auto legacyResourcesBefore = snapshot(m_legacyResources);

    QVERIFY(legacyProfileExists());
    QCOMPARE(legacyLanguage(), QStringLiteral("ja"));
    QVERIFY(resourcesToImport());
    QString error;
    QVERIFY2(importConfiguration(&error), qPrintable(error));
    QCOMPARE(state(), State::Importing);

    // Configuration: copied, kritarc paths rewritten, KXmlGui local files moved.
    const QString kritarc = QString::fromUtf8(readFile(KisSolsticePaths::configFilePath("kritarc")));
    const QString newResources = KisSolsticePaths::defaultResourceDir();
    QVERIFY(kritarc.contains("ResourceDirectory=" + newResources + "\n"));
    QVERIFY(kritarc.contains("AlwaysUseTemplate=" + newResources + "/templates/a.kra"));
    QVERIFY(kritarc.contains("OtherCopy=" + QDir::cleanPath(m_legacyResources) + " - Copy/x"));
    QString escaped = QDir::toNativeSeparators(newResources);
    escaped.replace("\\", "\\\\");
    QVERIFY2(kritarc.contains("mask_set=" + escaped + "\\\\pykrita"), qPrintable(kritarc));
    QCOMPARE(readFile(KisSolsticePaths::configFilePath("kritadisplayrc")),
             readFile(m_legacyConfig + "/kritadisplayrc"));
    QVERIFY(QFile::exists(KisSolsticePaths::xmlguiDataDir() + "/kxmlgui5/krita/custom.xmlgui"));

    // Resources: everything except database backups and kxmlgui5.
    const ResourceCopyPlan plan = planResourceCopy();
    QStringList files = plan.files;
    files.sort();
    QCOMPARE(
        files,
        QStringList({"KRITA_RESOURCE_VERSION", "brushes/a.gbr", "quickaccess/default.kqap", "resourcecache.sqlite"}));
    qint64 lastProgress = 0;
    QVERIFY2(copyResources(
                 plan,
                 [&](qint64 done) {
                     lastProgress = done;
                     return true;
                 },
                 &error),
             qPrintable(error));
    QCOMPARE(lastProgress, plan.bytes);
    QCOMPARE(state(), State::Ready);
    const auto copied = snapshot(newResources);
    for (const QString &file : plan.files) {
        QCOMPARE(copied.value(file), legacyResourcesBefore.value(file)); // content and modification time
    }

    // The Krita profile is unchanged.
    QCOMPARE(snapshot(m_legacyConfig), legacyConfigBefore);
    QCOMPARE(snapshot(m_legacyResources), legacyResourcesBefore);
}

void KisSolsticeProfileTest::testCustomResourceDirectoryIsKept()
{
    using namespace KisSolsticeProfile;
    createKritaProfile("ResourceDirectory=D:/Art/KritaResources");
    QVERIFY(!resourcesToImport());
    QVERIFY(importConfiguration());
    QCOMPARE(state(), State::Ready);
    QVERIFY(QString::fromUtf8(readFile(KisSolsticePaths::configFilePath("kritarc")))
                .contains("ResourceDirectory=D:/Art/KritaResources\n"));
    QVERIFY(QDir(KisSolsticePaths::defaultResourceDir()).isEmpty());
}

void KisSolsticeProfileTest::testCancelledCopy()
{
    using namespace KisSolsticeProfile;
    createKritaProfile(QByteArray());
    QVERIFY(importConfiguration());
    QCOMPARE(state(), State::Importing);
    QString error;
    QVERIFY(!copyResources(
        planResourceCopy(),
        [](qint64) {
            return false;
        },
        &error));
    QCOMPARE(
        error,
        builtInText(QStringLiteral("Cancelled"), QStringLiteral("キャンセルされました"), QStringLiteral("已取消")));
    QVERIFY(!QFileInfo::exists(KisSolsticePaths::defaultResourceDir()));
    abandonImport();
    QCOMPARE(state(), State::Missing);
    QVERIFY(!QFileInfo::exists(KisSolsticePaths::configDir()));
    QVERIFY(QFile::exists(m_legacyResources + "/resourcecache.sqlite"));
}

void KisSolsticeProfileTest::testTextLanguage()
{
    using namespace KisSolsticeProfile;
    // LANGIDs: en-US 0x0409, ja-JP 0x0411, zh-CN 0x0804, zh-SG 0x1004, zh-TW 0x0404
    QCOMPARE(textLanguageFor(QString(), 0x0409), TextLanguage::English);
    QCOMPARE(textLanguageFor(QString(), 0x0411), TextLanguage::Japanese);
    QCOMPARE(textLanguageFor(QString(), 0x0804), TextLanguage::ChineseSimplified);
    QCOMPARE(textLanguageFor(QString(), 0x1004), TextLanguage::ChineseSimplified);
    QCOMPARE(textLanguageFor(QString(), 0x0404), TextLanguage::English);
    QCOMPARE(textLanguageFor(QString(), 0), TextLanguage::English);
    // the Krita language override wins when it names a built-in language
    QCOMPARE(textLanguageFor(QStringLiteral("ja"), 0x0409), TextLanguage::Japanese);
    QCOMPARE(textLanguageFor(QStringLiteral("zh_CN"), 0x0411), TextLanguage::ChineseSimplified);
    QCOMPARE(textLanguageFor(QStringLiteral("zh_TW"), 0x0409), TextLanguage::English);
    QCOMPARE(textLanguageFor(QStringLiteral("en_US"), 0x0804), TextLanguage::ChineseSimplified);
}

SIMPLE_TEST_MAIN(KisSolsticeProfileTest)

#include "KisSolsticeProfileTest.moc"
