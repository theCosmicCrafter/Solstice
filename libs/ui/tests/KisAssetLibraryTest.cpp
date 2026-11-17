/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "assetlibrary/KisAssetLibraryBrowser.h"
#include "assetlibrary/KisAssetLibraryOperations.h"
#include "assetlibrary/KisAssetLibrarySettings.h"

#include <KoStore.h>
#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace
{
using Settings = KisAssetLibrarySettings;
using Cache = KisAssetLibraryThumbnailCache;

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray imageBytes(const QColor &color)
{
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
} // namespace

class KisAssetLibraryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testLegacySettings()
    {
        const auto json = QJsonDocument::fromJson(R"({
            "paths": [{"alias":"old", "path":"assets", "nested":true},
                      {"path":"second", "nested":true, "include_subfolders":false, "extensions":".PNG,SVG"},
                      {"alias":"empty"}],
            "font_size":15, "header_font_size":19,
            "window_width":830, "window_height":640, "splitter_sizes":[180,650],
            "right_panel_hidden":true, "expanded_window_width":830, "collapsed_window_width":190,
            "auto_columns":false, "columns":5, "thumbnail_size":160
        })")
                              .object();
        for (auto usage : {Settings::Usage::Docker, Settings::Usage::Welcome}) {
            Settings settings(usage);
            settings.fromJson(json);
            QCOMPARE(settings.paths.size(), 2);
            QCOMPARE(settings.paths[0].alias, QStringLiteral("old"));
            QVERIFY(settings.paths[0].includeSubfolders);
            QCOMPARE(settings.paths[0].extensions, QStringLiteral("kra,jpg,jpeg,png,svg"));
            QVERIFY(!settings.paths[1].includeSubfolders);
            QCOMPARE(settings.paths[1].extensions, QStringLiteral(".PNG,SVG"));
            QCOMPARE(settings.uiFontSize, 15);
            QCOMPARE(settings.headerFontSize, 19);
            QCOMPARE(settings.assetNameFontSize, 15);
            QVERIFY(!settings.autoColumns);
            QCOMPARE(settings.columns, 5);
            QCOMPARE(settings.thumbnailSize, 160);
            if (usage == Settings::Usage::Docker) {
                QCOMPARE(settings.windowWidth, 830);
                QCOMPARE(settings.windowHeight, 640);
                QCOMPARE(settings.splitterSizes, QList<int>({180, 650}));
                QVERIFY(settings.rightPanelHidden);
                QCOMPARE(settings.collapsedWindowWidth, 190);
            }
        }
    }

    void testConsumerValidationCompatibility()
    {
        const auto json = QJsonDocument::fromJson(R"({
            "columns":99,"thumbnail_size":999,"font_size":15,"ui_font_size":99,
            "header_font_size":0,"asset_name_font_size":0,"window_width":-1,"splitter_sizes":[1,-2]
        })")
                              .object();
        Settings dock;
        dock.fromJson(json);
        QCOMPARE(dock.columns, 3);
        QCOMPARE(dock.thumbnailSize, 512);
        QCOMPARE(dock.uiFontSize, 15);
        QCOMPARE(dock.headerFontSize, 15);
        QCOMPARE(dock.assetNameFontSize, 15);
        QCOMPARE(dock.windowWidth, 80);
        QCOMPARE(dock.splitterSizes, QList<int>({80, 0}));
        Settings welcome(Settings::Usage::Welcome);
        welcome.fromJson(json);
        QCOMPARE(welcome.columns, 12);
        QCOMPARE(welcome.thumbnailSize, 512);
        QCOMPARE(welcome.uiFontSize, 32);
        QCOMPARE(welcome.headerFontSize, 7);
        QCOMPARE(welcome.assetNameFontSize, 7);
        // Reloading absent values keeps docker state, but resets welcome defaults.
        dock.columns = welcome.columns = 6;
        dock.fromJson({});
        welcome.fromJson({});
        QCOMPARE(dock.columns, 6);
        QCOMPARE(welcome.columns, 3);
    }

    void testSaveScopeAndRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");
        const QJsonObject original{{"window_width", 923},
                                   {"right_panel_hidden", true},
                                   {"splitter_sizes", QJsonArray{200, 723}},
                                   {"unknown", "retain"},
                                   {"font_size", 14}};
        Settings welcome(Settings::Usage::Welcome);
        welcome.fromJson(original);
        welcome.paths.append({"art", "folder", true, "png,svg"});
        welcome.columns = 7;
        QVERIFY(welcome.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(saved.value("window_width"), original.value("window_width"));
        QCOMPARE(saved.value("splitter_sizes"), original.value("splitter_sizes"));
        QCOMPARE(saved.value("right_panel_hidden"), original.value("right_panel_hidden"));
        QCOMPARE(saved.value("unknown"), original.value("unknown"));
        QVERIFY(!saved.contains("font_size"));
        file.close();
        Settings dock;
        dock.load(path, {});
        QCOMPARE(dock.windowWidth, 923);
        QCOMPARE(dock.columns, 7);
        QCOMPARE(dock.paths.size(), 1);
        QCOMPARE(dock.paths[0].path, QStringLiteral("folder"));
        QVERIFY(dock.paths[0].includeSubfolders);
        dock.collapsedWindowWidth = 210;
        QVERIFY(dock.save(path));
        // Docker historically writes its complete known schema; Welcome edits
        // only display/path fields while carrying all other raw fields through.
        QVERIFY(!dock.toJson().contains("unknown"));
        welcome.load(path, {});
        QCOMPARE(welcome.toJson().value("collapsed_window_width").toInt(), 210);
        QCOMPARE(welcome.toJson(), dock.toJson());
    }

    void testViewsDoNotOverwriteEachOther()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");
        QVERIFY(writeFile(path, R"({"paths":[{"path":"first"}],"window_width":700,"unknown":"retain"})"));
        Settings dock;
        dock.load(path, {});
        Settings welcome(Settings::Usage::Welcome);
        welcome.load(path, {});
        QSignalSpy saved(KisAssetLibrarySettingsNotifier::instance(), &KisAssetLibrarySettingsNotifier::sigSaved);

        // Welcome adds a folder while the docker still holds the old list.
        welcome.paths.append({"", "second", false, "png"});
        welcome.columns = 5;
        QVERIFY(welcome.save(path));
        Settings stale;
        stale.load(path, {});
        QVERIFY(!stale.hasSameSharedValues(dock));

        // A docker resize stores only its layout.
        dock.windowWidth = 900;
        QVERIFY(dock.saveLayout(path));
        Settings reloaded;
        reloaded.load(path, {});
        QCOMPARE(reloaded.paths.size(), 2);
        QCOMPARE(reloaded.paths[1].path, QStringLiteral("second"));
        QCOMPARE(reloaded.columns, 5);
        QCOMPARE(reloaded.windowWidth, 900);

        // Welcome's later save keeps the docker layout written after it loaded.
        welcome.thumbnailSize = 200;
        QVERIFY(welcome.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(object.value("window_width").toInt(), 900);
        QCOMPARE(object.value("thumbnail_size").toInt(), 200);
        QCOMPARE(object.value("unknown").toString(), QStringLiteral("retain"));
        file.close();
        QCOMPARE(saved.count(), 3);

        // The docker adopts the shared values and keeps its own layout.
        dock.windowWidth = 640;
        reloaded.load(path, {});
        dock.copySharedValues(reloaded);
        QVERIFY(dock.hasSameSharedValues(reloaded));
        QCOMPARE(dock.thumbnailSize, 200);
        QCOMPARE(dock.windowWidth, 640);

        // Without a file, a layout save writes the docker's complete settings.
        const QString missing = dir.filePath("new.json");
        QVERIFY(dock.saveLayout(missing));
        Settings created;
        created.load(missing, {});
        QVERIFY(created.hasSameSharedValues(dock));
        QCOMPARE(created.windowWidth, 640);
        QVERIFY(!dock.saveLayout(dir.filePath("missing/config.json")));
        QCOMPARE(saved.count(), 4);
    }

    void testFileAndLegacyPrecedence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("config.json");
        const QByteArray legacy = R"({"columns":8,"paths":[{"path":"legacy","nested":true}]})";
        for (auto usage : {Settings::Usage::Docker, Settings::Usage::Welcome}) {
            Settings settings(usage);
            settings.load(path, legacy);
            QCOMPARE(settings.columns, 8);
            QCOMPARE(settings.paths[0].path, QStringLiteral("legacy"));
        }
        QVERIFY(writeFile(path, R"({"columns":4})"));
        Settings settings;
        settings.load(path, legacy);
        QCOMPARE(settings.columns, 4);
        QVERIFY(settings.paths.isEmpty());
        QVERIFY(writeFile(path, "invalid JSON"));
        Settings malformed;
        malformed.load(path, legacy);
        QCOMPARE(malformed.columns, 3);
        QVERIFY(malformed.paths.isEmpty()); // Readable malformed JSON does not import old data.
        QVERIFY(!settings.save(dir.filePath("missing/config.json")));
    }

    void testFolderEnumeration()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("nested"));
        for (const auto &name : {"b.PNG", "a.png", "drawing.svg", "ignore.txt", "nested/c.png"})
            QVERIFY(writeFile(dir.filePath(name), "fixture"));
        auto sections = KisAssetLibrary::collectSections(dir.path(), false, " .PNG , .svg ");
        QCOMPARE(sections.size(), 1);
        QCOMPARE(sections[0].files,
                 QStringList({dir.filePath("a.png"), dir.filePath("b.PNG"), dir.filePath("drawing.svg")}));
        QCOMPARE(sections[0].title, QFileInfo(dir.path()).fileName());
        sections = KisAssetLibrary::collectSections(dir.path(), true, "png");
        QCOMPARE(sections.size(), 2);
        QCOMPARE(sections[1].title, QStringLiteral("nested"));
        QCOMPARE(sections[1].files, QStringList({dir.filePath("nested/c.png")}));
        // Empty filters have always used the four raster extensions, without SVG.
        sections = KisAssetLibrary::collectSections(dir.path(), false, " , ");
        QCOMPARE(sections[0].files.size(), 2);
        QVERIFY(KisAssetLibrary::collectSections(dir.filePath("missing"), true, "png").isEmpty());
        QCOMPARE(KisAssetLibrary::thumbnailColumns(true, 3, 140, 8 + 2 * 174 - 10), 2);
        QCOMPARE(KisAssetLibrary::thumbnailColumns(true, 3, 140, 0), 1);
        QCOMPARE(KisAssetLibrary::thumbnailColumns(false, 5, 140, 1), 5);
    }

    void testKraPreviewPrecedence_data()
    {
        QTest::addColumn<QByteArray>("preview");
        QTest::addColumn<QByteArray>("thumbnail");
        QTest::addColumn<QColor>("expected");
        QTest::newRow("preview-first") << imageBytes(Qt::red) << imageBytes(Qt::green) << QColor(Qt::red);
        QTest::newRow("thumbnail-fallback") << QByteArray() << imageBytes(Qt::green) << QColor(Qt::green);
        QTest::newRow("invalid-preview") << QByteArray("invalid") << imageBytes(Qt::green) << QColor(Qt::green);
        QTest::newRow("merged-fallback") << QByteArray("invalid") << QByteArray("invalid") << QColor(Qt::blue);
    }

    void testKraPreviewPrecedence()
    {
        QFETCH(QByteArray, preview);
        QFETCH(QByteArray, thumbnail);
        QFETCH(QColor, expected);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("fixture.KRA");
        {
            QScopedPointer<KoStore> store(
                KoStore::createStore(path, KoStore::Write, "application/x-krita", KoStore::Zip));
            QVERIFY(store && !store->bad());
            const auto add = [&](const QString &name, const QByteArray &bytes) {
                if (bytes.isEmpty())
                    return;
                QVERIFY(store->open(name));
                QCOMPARE(store->write(bytes), qint64(bytes.size()));
                QVERIFY(store->close());
            };
            add("preview.png", preview);
            add("Thumbnails/thumbnail.png", thumbnail);
            add("mergedimage.png", imageBytes(Qt::blue));
        }
        Cache cache;
        for (auto style : {Cache::Placeholder::Docker, Cache::Placeholder::Welcome}) {
            const QPixmap result = cache.thumbnail(path, 80, 2.0, style);
            QCOMPARE(result.size(), QSize(160, 80));
            QCOMPARE(result.devicePixelRatio(), 2.0);
            QCOMPARE(result.toImage().pixelColor(0, 0), expected);
        }
    }

    void testThumbnailCacheInvalidation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("image.png");
        QVERIFY(writeFile(path, imageBytes(Qt::red)));
        Cache cache;
        const QPixmap first = cache.thumbnail(path, 80, 1.0);
        QCOMPARE(first.toImage().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(cache.thumbnail(path, 80, 1.0).cacheKey(), first.cacheKey());
        const QPixmap hidpi = cache.thumbnail(path, 80, 2.0);
        QCOMPARE(hidpi.size(), QSize(160, 80));
        QCOMPARE(hidpi.devicePixelRatio(), 2.0);
        QCOMPARE(cache.thumbnail(path, 100, 1.0).size(), QSize(100, 50));
        const QDateTime modified = QFileInfo(path).lastModified();
        QVERIFY(writeFile(path, imageBytes(Qt::blue)));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(modified.addSecs(2), QFileDevice::FileModificationTime));
        file.close();
        const QPixmap changed = cache.thumbnail(path, 80, 1.0);
        QCOMPARE(changed.toImage().pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(changed.cacheKey() != first.cacheKey());
        cache.clear();
        QVERIFY(cache.thumbnail(path, 80, 1.0).cacheKey() != changed.cacheKey());
        const QString missing = dir.filePath("missing.xyz");
        const auto docker = cache.thumbnail(missing, 140, 1.0, Cache::Placeholder::Docker);
        const auto welcome = cache.thumbnail(missing, 140, 1.0, Cache::Placeholder::Welcome);
        QCOMPARE(docker.size(), QSize(140, 140));
        QCOMPARE(welcome.size(), QSize(140, 140));
        QVERIFY(docker.toImage() != welcome.toImage()); // Keep the original placeholder font styles.
    }

    void testMissingDocumentDialogs()
    {
        QWidget parent;
        for (bool fileLayer : {false, true}) {
            bool shown = false;
            QTimer::singleShot(0, [&] {
                auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (message) {
                    shown = true;
                    QCOMPARE(message->parentWidget(), &parent);
                    message->accept();
                }
            });
            if (fileLayer)
                KisAssetLibrary::insertAsFileLayer("fixture.png", nullptr, &parent);
            else
                KisAssetLibrary::insertAsLayer("fixture.png", nullptr, &parent);
            QVERIFY(shown);
        }
    }

    void testFileNameExtensionAndCancel()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QWidget parent;
        const QString path = dir.filePath("original.png");
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->setTextValue("copy");
                dialog->accept();
            }
        });
        QCOMPARE(KisAssetLibrary::requestAssetPath(path, "test", &parent), dir.filePath("copy.png"));
        QTimer::singleShot(0, [&] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        QVERIFY(KisAssetLibrary::requestAssetPath(path, "test", &parent).isEmpty());
    }
};

QTEST_MAIN(KisAssetLibraryTest)
#include "KisAssetLibraryTest.moc"
