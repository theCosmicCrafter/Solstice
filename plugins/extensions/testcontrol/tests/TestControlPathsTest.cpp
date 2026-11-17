/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "TestControlPaths.h"

namespace
{
void touch(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
}
} // namespace

class TestControlPathsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void testRoot();
    void testOpenInsideRoot();
    void testRejectsEscapes();
    void testExportTargets();
    void testSymbolicLinkOutOfRoot();

private:
    QScopedPointer<QTemporaryDir> m_parent;
    QString m_root;
};

void TestControlPathsTest::init()
{
    m_parent.reset(new QTemporaryDir);
    QVERIFY(m_parent->isValid());
    QVERIFY(QDir(m_parent->path()).mkpath("root/sub"));
    m_root = TestControlPaths::testRoot(m_parent->path() + "/root");
    QVERIFY(!m_root.isEmpty());
    touch(m_root + "/image.png");
    touch(m_root + "/sub/inner.kra");
    touch(m_parent->path() + "/outside.png");
}

void TestControlPathsTest::testRoot()
{
    QCOMPARE(m_root, QFileInfo(m_parent->path() + "/root").canonicalFilePath());
    QVERIFY(TestControlPaths::testRoot(QString()).isEmpty());
    QVERIFY(TestControlPaths::testRoot("relative/dir").isEmpty());
    QVERIFY(TestControlPaths::testRoot(m_parent->path() + "/missing").isEmpty());
    QVERIFY(TestControlPaths::testRoot(m_root + "/image.png").isEmpty());

    const TestControlPaths::Resolved resolved = TestControlPaths::resolve(QString(), "image.png", true);
    QVERIFY(resolved.path.isEmpty());
    QVERIFY(resolved.error.contains("SOLSTICE_TEST_CONTROL_ROOT"));
}

void TestControlPathsTest::testOpenInsideRoot()
{
    TestControlPaths::Resolved resolved = TestControlPaths::resolve(m_root, "image.png", true);
    QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
    QCOMPARE(resolved.path, m_root + "/image.png");

    resolved = TestControlPaths::resolve(m_root, "sub/../sub/inner.kra", true);
    QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
    QCOMPARE(resolved.path, m_root + "/sub/inner.kra");

    resolved = TestControlPaths::resolve(m_root, "missing.png", true);
    QVERIFY(resolved.path.isEmpty());
    QVERIFY(!resolved.error.isEmpty());

    resolved = TestControlPaths::resolve(m_root, "sub", true);
    QVERIFY(resolved.path.isEmpty());
}

void TestControlPathsTest::testRejectsEscapes()
{
    const QStringList paths{QStringLiteral("../outside.png"),
                            QStringLiteral("sub/../../outside.png"),
                            QString(m_parent->path() + "/outside.png"),
                            QString(m_root + "/image.png"),
                            QString()};
    for (const QString &path : paths) {
        for (const bool mustExist : {true, false}) {
            const TestControlPaths::Resolved resolved = TestControlPaths::resolve(m_root, path, mustExist);
            QVERIFY2(resolved.path.isEmpty(), qPrintable(path + " -> " + resolved.path));
            QVERIFY(!resolved.error.isEmpty());
        }
    }
}

void TestControlPathsTest::testExportTargets()
{
    TestControlPaths::Resolved resolved = TestControlPaths::resolve(m_root, "new.png", false);
    QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
    QCOMPARE(resolved.path, m_root + "/new.png");

    resolved = TestControlPaths::resolve(m_root, "sub/new.png", false);
    QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
    QCOMPARE(resolved.path, m_root + "/sub/new.png");

    // An existing file may be replaced.
    resolved = TestControlPaths::resolve(m_root, "image.png", false);
    QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));

    resolved = TestControlPaths::resolve(m_root, "missing-dir/new.png", false);
    QVERIFY(resolved.path.isEmpty());

    resolved = TestControlPaths::resolve(m_root, "sub", false);
    QVERIFY(resolved.path.isEmpty());
}

void TestControlPathsTest::testSymbolicLinkOutOfRoot()
{
    if (!QFile::link(m_parent->path() + "/outside.png", m_root + "/link.png")
        || !QFile::link(m_parent->path(), m_root + "/linkdir")) {
        QSKIP("Cannot create symbolic links here");
    }
    for (const bool mustExist : {true, false}) {
        QVERIFY(TestControlPaths::resolve(m_root, "link.png", mustExist).path.isEmpty());
        QVERIFY(TestControlPaths::resolve(m_root, "linkdir/outside.png", mustExist).path.isEmpty());
    }
    QVERIFY(TestControlPaths::resolve(m_root, "linkdir/new.png", false).path.isEmpty());
}

QTEST_GUILESS_MAIN(TestControlPathsTest)

#include "TestControlPathsTest.moc"
