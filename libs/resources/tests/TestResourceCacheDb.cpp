/*
 * SPDX-FileCopyrightText: 2017 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "TestResourceCacheDb.h"
#include <simpletest.h>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QDir>
#include <QBuffer>
#include <QImage>
#include <QPainter>

#include <limits>

#include <KisResourceCacheDb.h>
#include <KisResourceLoaderRegistry.h>

void TestResourceCacheDb::initTestCase()
{
    QDir dbLocation(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (dbLocation.exists()) {
        QFile(dbLocation.path() + "/" + KisResourceCacheDb::resourceCacheDbFilename).remove();
        dbLocation.rmpath(dbLocation.path());
    }
}

void TestResourceCacheDb::testCreateDatabase()
{
    bool res = KisResourceCacheDb::initialize(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QVERIFY(res);
    QVERIFY(KisResourceCacheDb::isValid());

    QSqlDatabase sqlDb = QSqlDatabase::database();

    QStringList tables = QStringList() << "version_information"
                                       << "storage_types"
                                       << "resource_types"
                                       << "storages"
                                       << "tags"
                                       << "resources"
                                       << "versioned_resources"
                                       << "resource_tags";
    QStringList dbTables = sqlDb.tables();

    Q_FOREACH(const QString &table, tables) {
        QVERIFY2(dbTables.contains(table), table.toLatin1());
    }

    res = KisResourceCacheDb::initialize(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!res) {
        qWarning() << KisResourceCacheDb::lastError();
    }
    QVERIFY(res);
    QVERIFY(KisResourceCacheDb::isValid());
}

void TestResourceCacheDb::testLookupTables()
{
    QSqlQuery query;
    bool r = query.exec("SELECT COUNT(*) FROM storage_types");
    QVERIFY(r);
    QVERIFY(query.lastError() == QSqlError());
    query.first();
    QCOMPARE(query.value(0).toInt(), 7);

    r = query.exec("SELECT COUNT(*) FROM resource_types");
    QVERIFY(r);
    QVERIFY(query.lastError() == QSqlError());
    query.first();
    QVERIFY(query.value(0).toInt() == KisResourceLoaderRegistry::instance()->resourceTypes().count());
}

void TestResourceCacheDb::testMetaData()
{
    // Test adding metadata
    QMap<QString, QVariant> m1;
    m1["string"] = QString("bla");
    m1["bool"] = QVariant::fromValue<bool>(true);
    m1["int"] = QVariant::fromValue<int>(10);
    QImage img(50, 50, QImage::Format_RGB32);
    QPainter gc(&img);
    gc.fillRect(QRect(0, 0, 50, 50), QBrush(Qt::red));
    gc.end();
    m1["image"] = QVariant::fromValue<QImage>(img);

    bool r = KisResourceCacheDb::updateMetaDataForId(m1, 1, "test");
    QVERIFY(r);

    // Test retrieving metadata
    QMap<QString, QVariant> m2 = KisResourceCacheDb::metaDataForId(1, "test");
    QVERIFY(m1 == m2);

    // Test deleting metadata
    r = KisResourceCacheDb::updateMetaDataForId(QMap<QString, QVariant>(), 1, "test");
    QMap<QString, QVariant> m3 = KisResourceCacheDb::metaDataForId(1, "test");
    QVERIFY(m3.size() == 0);
}

namespace
{
// A PNG the way Krita 5.0 stored a preset thumbnail: the image, with the
// preset as text. The text does not compress, so the PNG is about as large.
QByteArray pngWithPresetText(const QImage &image, int textLength)
{
    QString text;
    text.reserve(textLength);
    quint32 state = 12345;
    for (int i = 0; i < textLength; ++i) {
        state = state * 1664525u + 1013904223u;
        text.append(QChar('A' + int((state >> 16) % 58)));
    }
    QImage withText = image;
    withText.setText("version", "5.0");
    withText.setText("preset", text);

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    withText.save(&buffer, "PNG");
    return png;
}

QByteArray pngOf(const QImage &image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

QImage noiseImage(int size, quint32 seed)
{
    QImage image(size, size, QImage::Format_ARGB32);
    quint32 state = seed;
    for (int y = 0; y < size; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size; ++x) {
            state = state * 1664525u + 1013904223u;
            line[x] = state | 0xff000000u;
        }
    }
    return image;
}
} // namespace

void TestResourceCacheDb::testPngWithoutTextChunks()
{
    const QImage image = noiseImage(40, 7);
    const QByteArray withText = pngWithPresetText(image, 100000);
    QVERIFY(withText.size() > 60000);
    QVERIFY(withText.contains("zTXt") || withText.contains("tEXt") || withText.contains("iTXt"));
    QVERIFY(!QImage::fromData(withText, "PNG").textKeys().isEmpty());

    const QByteArray stripped = KisResourceCacheDb::pngWithoutTextChunks(withText);
    QVERIFY(!stripped.isNull());
    QVERIFY(stripped.size() < withText.size() - 50000);
    QVERIFY(!stripped.contains("zTXt") && !stripped.contains("tEXt") && !stripped.contains("iTXt"));
    const QImage loaded = QImage::fromData(stripped, "PNG");
    QVERIFY(!loaded.isNull());
    QVERIFY(loaded.textKeys().isEmpty());
    QCOMPARE(loaded.convertToFormat(QImage::Format_ARGB32), image);

    // nothing left to remove
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(stripped).isNull());
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(pngOf(image)).isNull());

    // not a PNG, or a damaged one: left alone
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(QByteArray()).isNull());
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(QByteArray("not a png at all, just some bytes")).isNull());
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(withText.left(withText.size() / 2)).isNull());
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(withText.left(withText.size() - 6)).isNull());
    QByteArray wrongLength = withText;
    wrongLength[8] = char(0x7f); // the length of the first chunk
    QVERIFY(KisResourceCacheDb::pngWithoutTextChunks(wrongLength).isNull());
}

void TestResourceCacheDb::testRemoveTextFromPresetThumbnails()
{
    QVERIFY(KisResourceCacheDb::isValid());
    QVERIFY(KisResourceCacheDb::registerResourceType("paintoppresets"));
    QVERIFY(KisResourceCacheDb::registerResourceType("patterns"));

    QSqlQuery q;
    QVERIFY(
        q.exec("INSERT INTO storages (storage_type_id, location, timestamp, pre_installed, active) "
               "VALUES (1, 'thumbnail_cleanup_test', 0, 0, 1)"));
    const int storageId = q.lastInsertId().toInt();

    auto typeId = [](const QString &name) {
        QSqlQuery query;
        query.prepare("SELECT id FROM resource_types WHERE name = :name");
        query.bindValue(":name", name);
        return query.exec() && query.first() ? query.value(0).toInt() : -1;
    };
    const int presetType = typeId("paintoppresets");
    const int patternType = typeId("patterns");
    QVERIFY(presetType >= 0);
    QVERIFY(patternType >= 0);

    auto addResource = [storageId](int type, const QString &name, const QByteArray &thumbnail) {
        QSqlQuery query;
        query.prepare(
            "INSERT INTO resources (storage_id, resource_type_id, name, filename, tooltip, thumbnail, "
            "status, temporary, md5sum) "
            "VALUES (:storage, :type, :name, :filename, '', :thumbnail, 1, 0, :md5)");
        query.bindValue(":storage", storageId);
        query.bindValue(":type", type);
        query.bindValue(":name", name);
        query.bindValue(":filename", QString(name + ".kpp"));
        query.bindValue(":thumbnail", thumbnail);
        query.bindValue(":md5", name);
        return query.exec() ? query.lastInsertId().toInt() : -1;
    };
    auto thumbnailOf = [](int id) {
        QSqlQuery query;
        query.prepare("SELECT thumbnail FROM resources WHERE id = :id");
        query.bindValue(":id", id);
        return query.exec() && query.first() ? query.value(0).toByteArray() : QByteArray();
    };

    const QImage image = noiseImage(40, 21);
    const QByteArray largeWithText = pngWithPresetText(image, 300000);
    const QByteArray smallWithText = pngWithPresetText(image, 2000);
    const QByteArray largeWithoutText = pngOf(noiseImage(256, 3));
    QVERIFY(largeWithText.size() > 64 * 1024);
    QVERIFY(smallWithText.size() < 64 * 1024);
    QVERIFY(largeWithoutText.size() > 64 * 1024);

    const int presetLarge = addResource(presetType, "cleanup_large_with_text", largeWithText);
    const int presetSmall = addResource(presetType, "cleanup_small_with_text", smallWithText);
    const int presetClean = addResource(presetType, "cleanup_large_without_text", largeWithoutText);
    const int presetNoThumbnail = addResource(presetType, "cleanup_no_thumbnail", QByteArray());
    const int pattern = addResource(patternType, "cleanup_pattern_with_text", largeWithText);
    QVERIFY(presetLarge >= 0 && presetSmall >= 0 && presetClean >= 0 && presetNoThumbnail >= 0 && pattern >= 0);

    // without compacting the file
    const qint64 removed = KisResourceCacheDb::removeTextFromPresetThumbnails(std::numeric_limits<qint64>::max());
    const QByteArray cleaned = thumbnailOf(presetLarge);
    QCOMPARE(removed, qint64(largeWithText.size() - cleaned.size()));
    QVERIFY(removed > 200000);
    const QImage loaded = QImage::fromData(cleaned, "PNG");
    QVERIFY(loaded.textKeys().isEmpty());
    QCOMPARE(loaded.convertToFormat(QImage::Format_ARGB32), image);

    // only large preset thumbnails with text are touched
    QCOMPARE(thumbnailOf(presetSmall), smallWithText);
    QCOMPARE(thumbnailOf(presetClean), largeWithoutText);
    QVERIFY(thumbnailOf(presetNoThumbnail).isEmpty());
    QCOMPARE(thumbnailOf(pattern), largeWithText);

    // nothing more to do, also when the file is compacted
    QCOMPARE(KisResourceCacheDb::removeTextFromPresetThumbnails(0), qint64(0));
    QCOMPARE(thumbnailOf(presetLarge), cleaned);

    // a second round that compacts the file keeps every row
    const int presetSecond = addResource(presetType, "cleanup_second_with_text", largeWithText);
    QVERIFY(presetSecond >= 0);
    QVERIFY(KisResourceCacheDb::removeTextFromPresetThumbnails(0) > 200000);
    QCOMPARE(thumbnailOf(presetSecond), cleaned);
    QCOMPARE(thumbnailOf(presetLarge), cleaned);
    QCOMPARE(thumbnailOf(presetClean), largeWithoutText);
    QCOMPARE(thumbnailOf(pattern), largeWithText);

    // initialize() runs it: a database opened again is cleaned
    const int presetThird = addResource(presetType, "cleanup_third_with_text", largeWithText);
    QVERIFY(presetThird >= 0);
    QVERIFY(KisResourceCacheDb::initialize(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)));
    QCOMPARE(thumbnailOf(presetThird), cleaned);

    QVERIFY(q.exec(QString("DELETE FROM resources WHERE storage_id = %1").arg(storageId)));
    QVERIFY(q.exec(QString("DELETE FROM storages WHERE id = %1").arg(storageId)));
}

void TestResourceCacheDb::cleanupTestCase()
{
    QSqlDatabase::database().close();
    QDir dbLocation(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    bool res = QFile(dbLocation.path() + "/" + KisResourceCacheDb::resourceCacheDbFilename).remove();
    Q_ASSERT(res);
    res = dbLocation.rmpath(dbLocation.path());
    Q_ASSERT(res);
}

SIMPLE_TEST_MAIN(TestResourceCacheDb)

