/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lazytools/KisLazyToolsFastExport.h"
#include "lazytools/KisLazyToolsFastExportDialog.h"
#include "lazytools/KisLazyToolsFastExportPresets.h"
#include "lazytools/KisLazyToolsRenameDialog.h"
#include "lazytools/KisLazyToolsRenamePresets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <KoColorSpaceRegistry.h>
#include <KoStore.h>
#include <kundo2stack.h>
#include "KisDocument.h"
#include "KisPart.h"
#include "kis_config.h"
#include "kis_paint_layer.h"
#include "kis_paint_device.h"
#include "kis_properties_configuration.h"
#include "kis_default_bounds.h"
#include "kis_layer_composition.h"
#include "kis_selection.h"
#include "kis_undo_adapter.h"
#include "KisImageResolutionProxy.h"
#include <commands/kis_set_global_selection_command.h>
#include <testui.h>

namespace
{
namespace Presets = KisLazyToolsRenamePresets;
namespace Export = KisLazyToolsFastExport;
namespace Saved = KisLazyToolsFastExportPresets;

bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QPushButton *button(QWidget *widget, const QString &text)
{
    for (auto *candidate : widget->findChildren<QPushButton *>()) {
        if (candidate->text() == text)
            return candidate;
    }
    return nullptr;
}

KisDocument *document(const QString &path)
{
    auto *doc = KisPart::instance()->createDocument();
    // Export filters normally show their own options dialog. Test their output
    // in batch mode; production preserves each document's existing batch mode.
    doc->setFileBatchMode(true);
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(doc->createUndoStore(), 16, 12, cs, "export test");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());
    layer->paintDevice()->fill(image->bounds(), KoColor(QColor(80, 140, 200, 127), cs));
    doc->setCurrentImage(image);
    image->waitForDone();
    doc->setLocalFilePath(path);
    return doc;
}
void selectRect(KisDocument *doc, const QRect &rect)
{
    KisImageSP image = doc->image();
    KisSelectionSP selection =
        new KisSelection(new KisDefaultBounds(image), toQShared(new KisImageResolutionProxy(image)));
    selection->pixelSelection()->select(rect);
    selection->updateProjection();
    image->undoAdapter()->addCommand(new KisSetGlobalSelectionCommand(image, selection));
    image->waitForDone();
}

// Adds an opaque red layer above the document's layer and two comps: "Base"
// hides it, "Top" shows it. The red layer is visible afterwards.
QList<KisLayerCompositionSP> addLayerComps(KisDocument *doc)
{
    KisImageSP image = doc->image();
    const auto *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintLayerSP top = new KisPaintLayer(image, "top", OPACITY_OPAQUE_U8, cs);
    top->paintDevice()->fill(image->bounds(), KoColor(QColor(255, 0, 0), cs));
    image->addNode(top, image->root());
    QList<KisLayerCompositionSP> comps;
    for (const bool visible : {false, true}) {
        top->setVisible(visible);
        auto comp = toQShared(new KisLayerComposition(image, visible ? "Top" : "Base"));
        image->barrierLock();
        comp->store();
        image->unlock();
        image->addComposition(comp);
        comps << comp;
    }
    image->refreshGraphAsync();
    image->waitForDone();
    return comps;
}

// Settings whose one-off Destination is the document's own folder.
void saveSameFolderSettings()
{
    Export::Settings settings;
    settings.destination = 0;
    settings.save();
}

Saved::Preset layerCompPreset(const QString &name, const KisLayerCompositionSP &comp)
{
    Saved::Preset preset = Saved::newPreset(name);
    preset.layerComp = comp->uuid().toString(QUuid::WithoutBraces);
    preset.layerCompName = comp->name();
    return preset;
}
} // namespace

class KisLazyToolsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void presetCompatibility()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("name_color_list.txt");
        bool opened = true;
        QVERIFY(Presets::load(path, &opened).isEmpty());
        QVERIFY(!opened);
        const QByteArray legacy =
            "  Plain  \r\n\r\n Colored , bLuE \r\nGray, gray\r\nGrey, GREY\r\n"
            ", Red\r\nUnknown, cyan\r\nComma, name, Red\r\nPlain\r\n";
        QVERIFY(write(path, legacy));
        const auto presets = Presets::load(path, &opened);
        QVERIFY(opened);
        QCOMPARE(presets.size(), 7);
        const QStringList names{"Plain", "Colored", "Gray", "Grey", "Unknown", "Comma", "Plain"};
        const QList<int> labels{0, 1, 8, 8, 0, 0, 0};
        for (int i = 0; i < presets.size(); ++i) {
            QCOMPARE(presets[i].name, names[i]);
            QCOMPARE(presets[i].label, labels[i]);
        }
        QCOMPARE(read(path), legacy);
        for (int i = 0; i <= 8; ++i)
            QCOMPARE(Presets::colorLabelIndex(Presets::colorLabelName(i)), i);
        QVERIFY(Presets::colorLabelName(-1).isEmpty());
        QVERIFY(Presets::colorLabelName(9).isEmpty());

        Presets::append(path, "New", 6);
        const QByteArray appended = read(path);
        QVERIFY(appended.contains("  Plain  "));
        QVERIFY(appended.contains(" Colored , bLuE "));
        QVERIFY(appended.endsWith("New, Red"));
        Presets::append(path, "New", 6);
        QCOMPARE(read(path), appended); // Exact-line duplicates do not rewrite.
        Presets::append(path, "Colored", 1); // Existing differently spaced line remains distinct.
        QCOMPARE(Presets::load(path).size(), 9);
        const QString nested = dir.filePath("new/config/names.txt");
        Presets::append(nested, QString::fromUtf8("\xe7\xb7\x9a\xe7\x94\xbb"), 0);
        QCOMPARE(Presets::load(nested).first().name, QString::fromUtf8("\xe7\xb7\x9a\xe7\x94\xbb"));
    }

    void presetTextEditor()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("nested/config/names.txt");
        bool opened = true;
        QCOMPARE(Presets::readText(path, &opened), QString());
        QVERIFY(!opened);
        const QString text = QString::fromUtf8("  線画 , blue\n\nPlain\nBad, Teal\n,Red\nLast, gray");
        QVERIFY(Presets::writeText(path, text));
        QCOMPARE(Presets::readText(path, &opened), text);
        QVERIFY(opened);
        const auto presets = Presets::load(path);
        QCOMPARE(presets.size(), 4);
        QCOMPARE(presets[0].name, QString::fromUtf8("線画"));
        QCOMPARE(presets[0].label, 1);
        QCOMPARE(presets[2].label, 0);
        QCOMPARE(presets[3].label, 8);
        // The editor and the dialog's Save checkbox share one file.
        Presets::append(path, "Added", 6);
        QCOMPARE(Presets::readText(path), text + "\nAdded, Red");
        QVERIFY(Presets::writeText(path, QString()));
        QVERIFY(Presets::load(path).isEmpty());
        QVERIFY(!Presets::writeText(dir.path(), "a directory cannot be written"));
    }

    void exportSettingsAndPaths()
    {
        QTemporaryDir dir;
        auto config = KSharedConfig::openConfig(dir.filePath("test.ini"), KConfig::SimpleConfig);
        auto settings = Export::Settings::load(config);
        QCOMPARE(settings.folder, QString());
        QCOMPARE(settings.pngCompression, 6);
        QVERIFY(settings.pngAlpha);
        QCOMPARE(settings.jpegQuality, 90);
        QCOMPARE(settings.destination, 1);
        QVERIFY(!QFileInfo::exists(dir.filePath("test.ini")));
        auto group = config->group(QString());
        group.writeEntry("unrelated", 123);
        group.writeEntry("Solstice/FastExportFolder", "old folder");
        group.writeEntry("Solstice/FastExportPngCompression", 8);
        group.writeEntry("Solstice/FastExportPngAlpha", false);
        group.writeEntry("Solstice/FastExportJpegQuality", 71);
        group.writeEntry("Solstice/FastExportDestination", 7);
        settings = Export::Settings::load(config);
        QCOMPARE(settings.destination, 2);
        QCOMPARE(settings.folder, QString("old folder"));
        QCOMPARE(settings.pngCompression, 8);
        QVERIFY(!settings.pngAlpha);
        QCOMPARE(settings.jpegQuality, 71);
        settings.folder = "new folder";
        settings.pngCompression = 3;
        settings.pngAlpha = true;
        settings.jpegQuality = 88;
        settings.destination = 0;
        settings.save(config);
        config->reparseConfiguration();
        QCOMPARE(group.readEntry("unrelated", 0), 123);
        QCOMPARE(group.readEntry("Solstice/FastExportFolder", QString()), settings.folder);
        QCOMPARE(group.readEntry("Solstice/FastExportPngCompression", 0), 3);
        QVERIFY(group.readEntry("Solstice/FastExportPngAlpha", false));
        QCOMPARE(group.readEntry("Solstice/FastExportJpegQuality", 0), 88);
        QCOMPARE(group.readEntry("Solstice/FastExportDestination", -1), 0);
        const QString source = dir.filePath("paint.v2.kra");
        QCOMPARE(Export::outputPath(source, "png", {}), dir.filePath("paint.v2.png"));
        QCOMPARE(Export::outputPath(source, "jpg", dir.filePath("out")), dir.filePath("out/paint.v2.jpg"));
    }

    void renameDialog()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("names.txt");
        QVERIFY(write(path, "Preset, Red\nPlain"));
        auto group = KSharedConfig::openConfig()->group(QString());
        group.writeEntry("Solstice/RenameGridColumns", 0);
        group.writeEntry("Solstice/RenameDialogWidth", 1);
        group.writeEntry("Solstice/RenameDialogHeight", 1);
        QWidget parent;
        int applied = 0;
        QString name;
        int label = -1;
        auto apply = [&](const QString &value, int color) {
            ++applied;
            name = value;
            label = color;
            return true;
        };
        {
            QScopedPointer<QDialog> dialog(KisLazyToolsRenameDialog::create(&parent, apply, path));
            QCOMPARE(dialog->parentWidget(), &parent);
            QCOMPARE(dialog->size(), QSize(240, 200));
            auto *preset = button(dialog.data(), "Preset");
            QVERIFY(preset);
            preset->click();
            QCOMPARE(dialog->result(), int(QDialog::Accepted));
            QCOMPARE(applied, 1);
            QCOMPARE(name, QString("Preset"));
            QCOMPARE(label, 6);
            dialog->resize(550, 610);
        }
        QCOMPARE(group.readEntry("Solstice/RenameDialogWidth", 0), 550);
        QCOMPARE(group.readEntry("Solstice/RenameDialogHeight", 0), 610);
        QScopedPointer<QDialog> dialog(KisLazyToolsRenameDialog::create(&parent, apply, path));
        auto *input = dialog->findChild<QLineEdit *>();
        auto *combo = dialog->findChild<QComboBox *>();
        auto *save = dialog->findChild<QCheckBox *>();
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(input && combo && save && buttons);
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(applied, 1); // Empty names neither apply nor close.
        input->setText(" Manual ");
        combo->setCurrentIndex(2);
        save->setChecked(true);
        buttons->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(applied, 2);
        QCOMPARE(name, QString("Manual"));
        QCOMPARE(label, 2);
        QVERIFY(read(path).endsWith("Manual, Green"));
        const auto before = read(path);
        dialog.reset(KisLazyToolsRenameDialog::create(&parent, apply, path));
        dialog->findChild<QLineEdit *>()->setText("Cancelled");
        dialog->findChild<QCheckBox *>()->setChecked(true);
        dialog->reject();
        dialog->resize(560, 620);
        dialog.reset();
        QCOMPARE(read(path), before);
        QCOMPARE(applied, 2);
        QCOMPARE(group.readEntry("Solstice/RenameDialogWidth", 0), 560); // Cancel still remembers geometry.
        dialog.reset(KisLazyToolsRenameDialog::create(
            &parent,
            [](const QString &, int) {
                return false;
            },
            path));
        dialog->findChild<QLineEdit *>()->setText("No active node");
        dialog->findChild<QCheckBox *>()->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog->result(), int(QDialog::Rejected));
        QVERIFY(read(path).endsWith("No active node")); // Legacy save-before-apply order.
    }

    void exportDialog_data()
    {
        QTest::addColumn<bool>("all");
        QTest::addColumn<int>("destination");
        QTest::newRow("active-source") << false << 0;
        QTest::newRow("all-configured") << true << 1;
    }

    void exportDialog()
    {
        QFETCH(bool, all);
        QFETCH(int, destination);
        QTemporaryDir dir;
        Export::Settings baseline;
        baseline.save();
        QWidget parent;
        int resolves = 0;
        bool scope = !all;
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(
            &parent,
            [&](bool value) {
                ++resolves;
                scope = value;
                return QList<QPointer<KisDocument>>();
            },
            [](KisImageSP) {
                return false;
            });
        QCOMPARE(resolves, 0);
        QCOMPARE(dialog->parentWidget(), &parent);
        QVERIFY(dialog->testAttribute(Qt::WA_DeleteOnClose));
        const auto combos = dialog->findChildren<QComboBox *>();
        // Format, documents, destination, registered folder, area, then layer comp.
        QCOMPARE(combos.size(), 6);
        QCOMPARE(combos[0]->currentData().toString(), QString("png"));
        combos[0]->setCurrentIndex(1);
        combos[1]->setCurrentIndex(all ? 1 : 0);
        combos[2]->setCurrentIndex(destination);
        dialog->findChild<QLineEdit *>()->setText(" " + dir.path() + " ");
        const auto spins = dialog->findChildren<QSpinBox *>();
        QCOMPARE(spins.size(), 2);
        spins[0]->setValue(8);
        spins[1]->setValue(75);
        dialog->findChild<QCheckBox *>()->setChecked(false);
        auto *exportButton = button(dialog, "Export");
        QVERIFY(exportButton);
        exportButton->click();
        QCOMPARE(resolves, 1);
        QCOMPARE(scope, all);
        QTRY_VERIFY(dialog.isNull());
        const auto updated = Export::Settings::load();
        QCOMPARE(updated.folder, dir.path());
        QCOMPARE(updated.pngCompression, 8);
        QVERIFY(!updated.pngAlpha);
        QCOMPARE(updated.jpegQuality, 75);
    }

    void exportDialogCancellation()
    {
        Export::Settings baseline;
        baseline.folder = "unchanged";
        baseline.save();
        QWidget parent;
        int resolves = 0;
        auto documents = [&](bool) {
            ++resolves;
            return QList<QPointer<KisDocument>>();
        };
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(&parent, documents, [](KisImageSP) {
            return false;
        });
        const auto combos = dialog->findChildren<QComboBox *>();
        combos[2]->setCurrentIndex(1);
        dialog->findChild<QLineEdit *>()->clear();
        bool warningSeen = false;
        QTimer::singleShot(0, &parent, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warningSeen = true;
                box->accept();
            }
        });
        button(dialog, "Export")->click();
        QVERIFY(warningSeen);
        QCOMPARE(resolves, 0);
        QCOMPARE(Export::Settings::load().folder, baseline.folder);
        combos[2]->setCurrentIndex(2);
        bool chooserSeen = false;
        QTimer::singleShot(0, &parent, [&] {
            if (auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                chooserSeen = true;
                chooser->reject();
            }
        });
        button(dialog, "Export")->click();
        QVERIFY(chooserSeen);
        QCOMPARE(resolves, 0);
        QCOMPARE(Export::Settings::load().folder, baseline.folder);
        dialog->findChild<QSpinBox *>()->setValue(0);
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(Export::Settings::load().pngCompression, 6);
    }

    void explorerFolders()
    {
        QTemporaryDir dir;
        auto config = KSharedConfig::openConfig(dir.filePath("test.ini"), KConfig::SimpleConfig);
        QCOMPARE(Export::explorerFolders(config), QStringList());
        // Native separators: backslashes are separators only on Windows; elsewhere
        // they are part of a file name and stay.
        Export::setExplorerFolders({" " + QDir::toNativeSeparators("C:/Art/Root") + " ", "", "C:/Art/Root", "D:/Work"},
                                   config);
        config->reparseConfiguration();
        QCOMPARE(config->group(QString()).readEntry("Solstice/FastExportExplorerFolders", QStringList()),
                 QStringList({"C:/Art/Root", "D:/Work"}));
        QCOMPARE(Export::explorerFolders(config), QStringList({"C:/Art/Root", "D:/Work"}));
        // The export dialog saves Settings on Export; that must not touch the list.
        Export::Settings().save(config);
        QCOMPARE(Export::explorerFolders(config), QStringList({"C:/Art/Root", "D:/Work"}));
#ifndef Q_OS_WIN
        Export::setExplorerFolders({"/art/a\\b", "/art/A\\b"}, config);
        QCOMPARE(Export::explorerFolders(config), QStringList({"/art/a\\b", "/art/A\\b"}));
#endif
    }

    void registeredFolderBrowse()
    {
        QTemporaryDir dir;
        const QString missing = dir.filePath("missing");
        const QString fallback = dir.filePath("fallback");
        const QString root = dir.filePath("root");
        QVERIFY(QDir().mkpath(fallback));
        QVERIFY(QDir().mkpath(root + "/sub"));
        Export::Settings settings;
        settings.folder = fallback;
        settings.destination = 0;
        settings.save();
        Export::setExplorerFolders({});
        QWidget parent;
        auto documents = [](bool) {
            return QList<QPointer<KisDocument>>();
        };
        auto wait = [](KisImageSP) {
            return false;
        };
        // Browse starts in the chosen registered folder, or the export folder
        // when there is none; it reports where the chooser opened.
        auto browse = [&](QDialog *dialog, const QString &selection) {
            QString start;
            QTimer::singleShot(0, &parent, [&] {
                if (auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    start = chooser->directory().absolutePath();
                    if (selection.isEmpty()) {
                        chooser->reject();
                    } else {
                        chooser->setDirectory(selection);
                        QMetaObject::invokeMethod(chooser, "accept");
                    }
                }
            });
            button(dialog, "Browse…")->click();
            return start;
        };

        QPointer<QDialog> empty = KisLazyToolsFastExportDialog::create(&parent, documents, wait);
        QCOMPARE(empty->findChildren<QComboBox *>().size(), 6);
        QVERIFY(!empty->findChildren<QComboBox *>()[3]->isEnabled());
        QVERIFY(button(empty, "Browse…")->isEnabled());
        QVERIFY(!button(empty, "Open in Explorer"));
        QCOMPARE(browse(empty, {}), fallback);
        QCOMPARE(empty->findChild<QLineEdit *>()->text(), fallback);
        QCOMPARE(empty->findChildren<QComboBox *>()[2]->currentData().toInt(), 0);
        empty->reject();
        QTRY_VERIFY(empty.isNull());

        Export::setExplorerFolders({missing, root});
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(&parent, documents, wait);
        const auto combos = dialog->findChildren<QComboBox *>();
        QComboBox *folders = combos[3];
        auto *folderEdit = dialog->findChild<QLineEdit *>();
        QVERIFY(folders->isEnabled());
        QCOMPARE(folders->count(), 2);
        QCOMPARE(folders->itemData(0).toString(), missing);
        QCOMPARE(folders->itemText(1), QDir::toNativeSeparators(root));
        QCOMPARE(browse(dialog, {}), fallback);
        folders->setCurrentIndex(1);
        QCOMPARE(browse(dialog, root + "/sub"), root);
        QCOMPARE(folderEdit->text(), root + "/sub");
        QCOMPARE(combos[2]->currentData().toInt(), 1);

        // A folder dropped from the file manager becomes the configured export folder.
        combos[2]->setCurrentIndex(0);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(fallback)});
        QDragEnterEvent enter(QPoint(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(folderEdit, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(folderEdit, &drop);
        QCOMPARE(folderEdit->text(), fallback);
        QCOMPARE(combos[2]->currentData().toInt(), 1);
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
        Export::setExplorerFolders({});
    }

    void exportDialogDestination()
    {
        QTemporaryDir dir;
        QScopedPointer<KisDocument> doc(document(dir.filePath("dest.kra")));
        Export::Settings().save();
        QWidget parent;
        auto open = [&] {
            return QPointer<QDialog>(KisLazyToolsFastExportDialog::create(
                &parent,
                [&](bool) {
                    return QList<QPointer<KisDocument>>({doc.data()});
                },
                [](KisImageSP) {
                    return true;
                },
                doc.data()));
        };
        // The configured export folder is the default.
        QPointer<QDialog> dialog = open();
        auto *destination = dialog->findChildren<QComboBox *>()[2];
        QCOMPARE(destination->currentData().toInt(), 1);
        // A choice is kept without exporting.
        destination->setCurrentIndex(destination->findData(2));
        delete dialog;
        QCOMPARE(Export::Settings::load().destination, 2);
        dialog = open();
        destination = dialog->findChildren<QComboBox *>()[2];
        QCOMPARE(destination->currentData().toInt(), 2);

        // A saved export's destination is its own and does not change it.
        destination->setCurrentIndex(destination->findData(1));
        auto *list = dialog->findChild<QListWidget *>();
        button(dialog, "Add")->click();
        QCOMPARE(list->currentRow(), 1);
        destination->setCurrentIndex(destination->findData(0));
        QCOMPARE(Saved::presets(doc.data())[0].destination, Saved::Destination::SameFolder);
        QCOMPARE(Export::Settings::load().destination, 1);
        list->setCurrentRow(0);
        QCOMPARE(destination->currentData().toInt(), 1);
        delete dialog;
        dialog = open();
        QCOMPARE(dialog->findChildren<QComboBox *>()[2]->currentData().toInt(), 1);
        delete dialog;
    }

    void realExport_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<bool>("alpha");
        QTest::addColumn<bool>("modified");
        QTest::addColumn<bool>("sameFolder");
        QTest::newRow("png-alpha-source") << QString("png") << true << true << true;
        QTest::newRow("png-opaque-configured") << QString("png") << false << false << false;
        QTest::newRow("jpeg-source") << QString("jpg") << false << false << true;
        QTest::newRow("jpeg-configured-modified") << QString("jpg") << false << true << false;
    }

    void realExport()
    {
        QFETCH(QString, extension);
        QFETCH(bool, alpha);
        QFETCH(bool, modified);
        QFETCH(bool, sameFolder);
        QTemporaryDir dir;
        const QString source = dir.filePath("original.v2.kra");
        const QString target = dir.filePath("exports");
        QVERIFY(QDir().mkpath(target));
        QScopedPointer<KisDocument> doc(document(source));
        doc->setModified(modified);
        const QString caption = doc->caption();
        Export::Settings settings;
        settings.pngAlpha = alpha;
        settings.pngCompression = 9;
        settings.jpegQuality = 92;
        int waits = 0;
        const QString folder = sameFolder ? QString() : target;
        auto result = Export::run({doc.data()}, extension, folder, settings, [&](KisImageSP image) {
            ++waits;
            image->waitForDone();
            return true;
        });
        QCOMPARE(waits, 1);
        QCOMPARE(result.exported, 1);
        QVERIFY(result.skipped.isEmpty());
        QImage output(Export::outputPath(source, extension, folder));
        QVERIFY(!output.isNull());
        QCOMPARE(output.size(), QSize(16, 12));
        QCOMPARE(output.pixelColor(5, 5).alpha(), extension == "png" && alpha ? 127 : 255);
        QCOMPARE(doc->localFilePath(), source);
        QCOMPARE(doc->caption(), caption);
        QCOMPARE(doc->isModified(), modified);
    }

    void exportOptionsKeepSavedFormatOptions()
    {
        auto root = KSharedConfig::openConfig()->group(QString());
        root.deleteEntry("ExportConfiguration-image/png");
        root.deleteEntry("ExportConfiguration-image/jpeg");
        Export::Settings settings;
        settings.pngCompression = 7;
        settings.pngAlpha = true;
        settings.jpegQuality = 55;

        // Nothing saved yet: only this dialog's values.
        auto png = Export::exportOptions("png", settings);
        QCOMPARE(png->getInt("compression"), 7);
        QVERIFY(png->getBool("alpha"));
        QVERIFY(!png->hasProperty("forceSRGB"));
        QCOMPARE(Export::exportOptions("jpg", settings)->getInt("quality"), 55);

        // Options saved by File > Export or a previous export are kept; this
        // dialog's own options win.
        KisPropertiesConfigurationSP savedPng(new KisPropertiesConfiguration);
        savedPng->setProperty("forceSRGB", true);
        savedPng->setProperty("saveSRGBProfile", false);
        savedPng->setProperty("compression", 1);
        savedPng->setProperty("alpha", false);
        KisConfig(false).setExportConfiguration("image/png", savedPng);
        KisPropertiesConfigurationSP savedJpeg(new KisPropertiesConfiguration);
        savedJpeg->setProperty("progressive", true);
        savedJpeg->setProperty("quality", 10);
        KisConfig(false).setExportConfiguration("image/jpeg", savedJpeg);

        png = Export::exportOptions("png", settings);
        QVERIFY(png->getBool("forceSRGB", false));
        QVERIFY(!png->getBool("saveSRGBProfile", true));
        QCOMPARE(png->getInt("compression"), 7);
        QVERIFY(png->getBool("alpha"));
        auto jpeg = Export::exportOptions("jpg", settings);
        QVERIFY(jpeg->getBool("progressive", false));
        QCOMPARE(jpeg->getInt("quality"), 55);
        QVERIFY(!jpeg->hasProperty("forceSRGB"));

        root.deleteEntry("ExportConfiguration-image/png");
        root.deleteEntry("ExportConfiguration-image/jpeg");
    }

    void exportNames()
    {
        QCOMPARE(Export::normalizedBaseName("  cover  ", "png"), QString("cover"));
        QCOMPARE(Export::normalizedBaseName("cover.PNG", "png"), QString("cover"));
        QCOMPARE(Export::normalizedBaseName("cover.jpeg", "jpg"), QString("cover"));
        QCOMPARE(Export::normalizedBaseName("cover.v2", "png"), QString("cover.v2"));
        QCOMPARE(Export::normalizedBaseName(".png", "png"), QString(".png"));
        QCOMPARE(Export::normalizedBaseName("   ", "png"), QString());
        QVERIFY(Export::isValidBaseName(QString::fromUtf8("表紙 v2.final")));
        const QStringList invalidNames =
            {"a/b", "a\\b", "a:b", "a*", "a?", "a\"b", "<a>", "a|b", ".", "..", "a.", "a ", "a\tb", QString()};
        for (const QString &invalid : invalidNames) {
            QVERIFY2(!Export::isValidBaseName(invalid), qPrintable(invalid));
        }
        QTemporaryDir dir;
        const QString source = dir.filePath("paint.v2.kra");
        QCOMPARE(Export::outputPath(source, "png", {}, "cover"), dir.filePath("cover.png"));
        QCOMPARE(Export::outputPath(source, "jpg", dir.filePath("out"), "cover.v3"), dir.filePath("out/cover.v3.jpg"));
    }

    void exportDialogName()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("original.kra");
        QScopedPointer<KisDocument> doc(document(source));
        saveSameFolderSettings();
        QWidget parent;
        bool allDocuments = false;
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(
            &parent,
            [&](bool all) {
                allDocuments = all;
                return QList<QPointer<KisDocument>>({doc.data()});
            },
            [](KisImageSP image) {
                image->waitForDone();
                return true;
            });
        QLineEdit *name = nullptr;
        for (auto *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == "Same as the document")
                name = edit;
        }
        QVERIFY(name);
        QVERIFY(dialog->findChild<QLineEdit *>() != name);
        auto *clear = button(dialog, "Clear");
        QVERIFY(clear);
        QVERIFY(!clear->isEnabled());
        name->setText("cover");
        QVERIFY(clear->isEnabled());
        clear->click();
        QVERIFY(name->text().isEmpty());
        QVERIFY(!clear->isEnabled());

        // The name applies only to the active document.
        const auto combos = dialog->findChildren<QComboBox *>();
        name->setText("cover");
        combos[1]->setCurrentIndex(1);
        QVERIFY(!name->isEnabled());
        QVERIFY(!clear->isEnabled());
        combos[1]->setCurrentIndex(0);
        QVERIFY(name->isEnabled());
        QVERIFY(clear->isEnabled());

        name->setText("bad:name");
        bool warningSeen = false;
        QTimer::singleShot(0, &parent, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warningSeen = true;
                box->accept();
            }
        });
        button(dialog, "Export")->click();
        QVERIFY(warningSeen);
        QVERIFY(!QFileInfo::exists(dir.filePath("bad.png")));

        name->setText(QString::fromUtf8(" 表紙.png "));
        button(dialog, "Export")->click();
        QVERIFY(!allDocuments);
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(!QImage(dir.filePath(QString::fromUtf8("表紙.png"))).isNull());
        QVERIFY(!QFileInfo::exists(dir.filePath("original.png")));
        QCOMPARE(doc->localFilePath(), source);
    }

    void exportRegion()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("region.kra");
        const QString target = dir.filePath("out");
        QVERIFY(QDir().mkpath(target));
        QScopedPointer<KisDocument> doc(document(source));
        doc->setModified(false);
        auto wait = [](KisImageSP image) {
            image->waitForDone();
            return true;
        };
        QScopedPointer<KisDocument> unnamed(document({}));
        selectRect(unnamed.data(), QRect(1, 1, 2, 2));
        QCOMPARE(Export::exportRegion(unnamed.data(), "png", target, {}, wait), Export::RegionResult::Unnamed);
        QCOMPARE(Export::exportRegion(nullptr, "png", target, {}, wait), Export::RegionResult::Unnamed);
        QCOMPARE(Export::exportRegion(doc.data(), "png", target, {}, wait), Export::RegionResult::NoSelection);
        QVERIFY(QDir(target).isEmpty());

        selectRect(doc.data(), QRect(2, 3, 5, 4));
        doc->setModified(false);
        QCOMPARE(Export::exportRegion(doc.data(), "png", target, {}, wait, "part"), Export::RegionResult::Exported);
        QImage png(QDir(target).filePath("part.png"));
        QCOMPARE(png.size(), QSize(5, 4));
        QCOMPARE(png.pixelColor(0, 0).alpha(), 127);
        QCOMPARE(png.pixelColor(4, 3).alpha(), 127);
        QCOMPARE(Export::exportRegion(doc.data(), "jpg", {}, {}, wait), Export::RegionResult::Exported);
        QCOMPARE(QImage(dir.filePath("region.jpg")).size(), QSize(5, 4));
        QCOMPARE(doc->localFilePath(), source);
        QVERIFY(!doc->isModified());
        QCOMPARE(doc->image()->size(), QSize(16, 12));
        QVERIFY(doc->image()->globalSelection());
        QCOMPARE(Export::exportRegion(doc.data(), "png", dir.filePath("missing/folder"), {}, wait),
                 Export::RegionResult::Failed);
    }

    void exportDialogRegion()
    {
        QTemporaryDir dir;
        QScopedPointer<KisDocument> doc(document(dir.filePath("original.kra")));
        saveSameFolderSettings();
        QWidget parent;
        QList<bool> scopes;
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(
            &parent,
            [&](bool all) {
                scopes << all;
                return QList<QPointer<KisDocument>>({doc.data()});
            },
            [](KisImageSP image) {
                image->waitForDone();
                return true;
            });
        auto *region = button(dialog, "Export Region");
        QVERIFY(region);
        const auto combos = dialog->findChildren<QComboBox *>();
        combos[1]->setCurrentIndex(1);
        QVERIFY(!region->isEnabled());
        combos[1]->setCurrentIndex(0);
        QVERIFY(region->isEnabled());

        bool warningSeen = false;
        QTimer::singleShot(0, &parent, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warningSeen = true;
                box->accept();
            }
        });
        region->click();
        QVERIFY(warningSeen);
        QVERIFY(!dialog.isNull());

        selectRect(doc.data(), QRect(0, 0, 3, 2));
        for (auto *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == "Same as the document")
                edit->setText("crop");
        }
        region->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(scopes, QList<bool>({false, false}));
        QCOMPARE(QImage(dir.filePath("crop.png")).size(), QSize(3, 2));
        QVERIFY(!QFileInfo::exists(dir.filePath("original.png")));
    }

    void exportSkippedDocuments()
    {
        QTemporaryDir dir;
        QScopedPointer<KisDocument> named(document(dir.filePath("named.kra")));
        QScopedPointer<KisDocument> unnamed(document({}));
        int waits = 0;
        auto result = Export::run({nullptr, unnamed.data(), named.data()}, "png", {}, {}, [&](KisImageSP) {
            ++waits;
            return false;
        });
        QCOMPARE(waits, 1);
        QCOMPARE(result.exported, 0);
        QCOMPARE(result.skipped, QStringList({"Unknown document", unnamed->caption(), named->caption()}));
        QVERIFY(!QFileInfo::exists(dir.filePath("named.png")));
        result = Export::run({named.data()}, "png", dir.filePath("missing/folder"), {}, [](KisImageSP) {
            return true;
        });
        QCOMPARE(result.exported, 0);
        QCOMPARE(result.skipped, QStringList({named->caption()}));
    }

    void presetXml()
    {
        QCOMPARE(Saved::toXml({}), QByteArray());
        QVERIFY(Saved::fromXml({}).isEmpty());
        QVERIFY(Saved::fromXml("not xml").isEmpty());
        QVERIFY(Saved::fromXml("<other><preset format=\"png\"/></other>").isEmpty());

        Saved::Preset web = Saved::newPreset(QString::fromUtf8("Web \xe6\x9c\xac"));
        web.enabled = false;
        web.format = "jpg";
        web.jpegQuality = 77;
        web.formatOptions =
            "<!DOCTYPE params>\n<params version=\"1\"><param name=\"progressive\" "
            "type=\"bool\"><![CDATA[true]]></param></params>\n";
        web.destination = Saved::Destination::Folder;
        web.folder = "../web out";
        web.folderRelative = true;
        web.nameRule = Saved::NameRule::Fixed;
        web.fixedName = "cover";
        web.area = Saved::Area::Selection;
        Saved::Preset png = Saved::newPreset("Godot");
        png.pngCompression = 3;
        png.pngAlpha = false;
        png.layerComp = QUuid::createUuid().toString(QUuid::WithoutBraces);
        png.layerCompName = QString::fromUtf8("Comp \xe6\x9c\xac");
        QVERIFY(!web.id.isEmpty() && web.id != png.id);
        const QByteArray data = Saved::toXml({web, png});
        QCOMPARE(Saved::fromXml(data), QList<Saved::Preset>({web, png}));
        // Exports of the layers as they are save what they saved before.
        QVERIFY(!Saved::toXml({web}).contains("layerComp"));

        // Newer or damaged data: unknown parts are ignored, unknown formats
        // dropped, ids made unique and numbers clamped.
        const auto read = Saved::fromXml(
            "<fastExport version=\"7\"><future/>"
            "<preset id=\"a\" name=\"One\" format=\"png\" pngCompression=\"42\" extra=\"x\"><more/></preset>"
            "<preset id=\"a\" name=\"Two\" format=\"jpg\" jpegQuality=\"-5\"/>"
            "<preset name=\"Three\" format=\"webp\"/>"
            "<preset name=\"Four\" format=\"png\" enabled=\"0\"/></fastExport>");
        QCOMPARE(read.size(), 3);
        QCOMPARE(read[0].id, QString("a"));
        QCOMPARE(read[0].pngCompression, 9);
        QVERIFY(read[0].enabled);
        QVERIFY(read[1].id != "a" && !read[1].id.isEmpty());
        QCOMPARE(read[1].jpegQuality, 0);
        QCOMPARE(read[2].name, QString("Four"));
        QVERIFY(!read[2].enabled);
        QVERIFY(!read[2].id.isEmpty());
        QCOMPARE(read[2].destination, Saved::Destination::SameFolder);
        QCOMPARE(read[2].nameRule, Saved::NameRule::DocumentName);
        QCOMPARE(read[2].area, Saved::Area::WholeImage);
    }

    void presetPaths()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("art/hero.v2.kra");
        Saved::Preset preset = Saved::newPreset("p");
        QCOMPARE(Saved::outputPath(preset, source), dir.filePath("art/hero.v2.png"));
        QCOMPARE(Saved::outputPath(preset, {}), QString());
        // A layer comp's name follows the document's.
        QCOMPARE(Saved::outputPath(preset, source, "001"), dir.filePath("art/hero.v2_001.png"));
        QCOMPARE(Saved::outputPath(preset, source, "a:b"), QString());

        preset.destination = Saved::Destination::Folder;
        QCOMPARE(Saved::outputPath(preset, source), QString());
        preset.folder = dir.filePath("out");
        preset.format = "jpg";
        QCOMPARE(Saved::outputPath(preset, source), dir.filePath("out/hero.v2.jpg"));

        preset.folderRelative = true;
        preset.folder = Saved::storedFolder(dir.filePath("game/gfx"), true, source);
        QCOMPARE(preset.folder, QString("../game/gfx"));
        QCOMPARE(Saved::resolvedFolder(preset, source), dir.filePath("game/gfx"));
        QCOMPARE(Saved::resolvedFolder(preset, {}), QString());
        // The relative folder follows the document when both move.
        QCOMPARE(Saved::resolvedFolder(preset, dir.filePath("moved/art/hero.kra")), dir.filePath("moved/game/gfx"));
        QCOMPARE(Saved::storedFolder(dir.filePath("x"), false, source), dir.filePath("x"));
        QCOMPARE(Saved::storedFolder(dir.filePath("x"), true, {}), dir.filePath("x"));

        preset.nameRule = Saved::NameRule::Fixed;
        preset.fixedName = "sprite.JPG";
        QCOMPARE(Saved::outputPath(preset, source), dir.filePath("game/gfx/sprite.jpg"));
        QCOMPARE(Saved::outputPath(preset, source, "001"), dir.filePath("game/gfx/sprite.jpg"));
        preset.fixedName = "bad/name";
        QCOMPARE(Saved::outputPath(preset, source), QString());
        preset.fixedName.clear();
        QCOMPARE(Saved::outputPath(preset, source), QString());
    }

    void presetOptions()
    {
        auto root = KSharedConfig::openConfig()->group(QString());
        root.writeEntry("ExportConfiguration-image/png",
                        "<!DOCTYPE params>\n<params version=\"1\"><param name=\"forceSRGB\" "
                        "type=\"bool\"><![CDATA[false]]></param></params>\n");
        Saved::Preset preset = Saved::newPreset("p");
        preset.pngCompression = 2;
        preset.pngAlpha = false;
        auto options = Saved::exportOptions(preset);
        QCOMPARE(options->getInt("compression"), 2);
        QCOMPARE(options->getBool("alpha"), false);
        QCOMPARE(options->getBool("forceSRGB", true), false);

        // The preset's own options win over the remembered ones.
        preset.formatOptions =
            "<!DOCTYPE params>\n<params version=\"1\"><param name=\"forceSRGB\" "
            "type=\"bool\"><![CDATA[true]]></param></params>\n";
        options = Saved::exportOptions(preset);
        QCOMPARE(options->getBool("forceSRGB", false), true);
        QCOMPARE(options->getInt("compression"), 2);
        root.deleteEntry("ExportConfiguration-image/png");
    }

    void presetDocumentStorage()
    {
        QTemporaryDir dir;
        const QString kra = dir.filePath("stored.kra");
        QScopedPointer<KisDocument> doc(document(kra));
        doc->setModified(false);
        const int undoCount = doc->undoStack()->count();
        QVERIFY(Saved::canStore(doc.data()));
        Saved::Preset preset = Saved::newPreset("Godot");
        preset.format = "jpg";
        Saved::setPresets(doc.data(), {preset});
        QTRY_VERIFY(doc->isModified());
        QCOMPARE(doc->undoStack()->count(), undoCount);
        QCOMPARE(Saved::presets(doc.data()), QList<Saved::Preset>({preset}));

        // Saving works on a clone.
        QScopedPointer<KisDocument> clone(doc->clone());
        QCOMPARE(Saved::presets(clone.data()), QList<Saved::Preset>({preset}));

        // An unchanged list does not mark the document modified.
        doc->setModified(false);
        Saved::setPresets(doc.data(), {preset});
        QTest::qWait(50);
        QVERIFY(!doc->isModified());

        QVERIFY(doc->exportDocumentSync(kra, "application/x-krita"));
        QScopedPointer<KisDocument> loaded(KisPart::instance()->createDocument());
        loaded->setFileBatchMode(true);
        QVERIFY(loaded->loadNativeFormat(kra));
        QCOMPARE(Saved::presets(loaded.data()), QList<Saved::Preset>({preset}));
        QVERIFY(!loaded->isModified());
        {
            QScopedPointer<KoStore> store(KoStore::createStore(kra, KoStore::Read));
            QVERIFY(store && store->hasFile("solstice/fastexport.xml"));
        }

        // Without presets the file has no entry, like files saved before.
        const QString plain = dir.filePath("plain.kra");
        QScopedPointer<KisDocument> empty(document(plain));
        QVERIFY(empty->exportDocumentSync(plain, "application/x-krita"));
        {
            QScopedPointer<KoStore> store(KoStore::createStore(plain, KoStore::Read));
            QVERIFY(store && !store->hasFile("solstice/fastexport.xml"));
        }
        QScopedPointer<KisDocument> loadedPlain(KisPart::instance()->createDocument());
        loadedPlain->setFileBatchMode(true);
        QVERIFY(loadedPlain->loadNativeFormat(plain));
        QVERIFY(Saved::presets(loadedPlain.data()).isEmpty());

        // A PNG document keeps them only in memory, so it is not marked modified.
        QScopedPointer<KisDocument> png(document(dir.filePath("flat.png")));
        png->setModified(false);
        QVERIFY(!Saved::canStore(png.data()));
        Saved::setPresets(png.data(), {preset});
        QTest::qWait(50);
        QVERIFY(!png->isModified());
        QCOMPARE(Saved::presets(png.data()).size(), 1);
        QScopedPointer<KisDocument> unsaved(document({}));
        QVERIFY(Saved::canStore(unsaved.data()));
    }

    void presetRun()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("art/hero.kra");
        QVERIFY(QDir().mkpath(dir.filePath("art")));
        QVERIFY(QDir().mkpath(dir.filePath("game/gfx")));
        QScopedPointer<KisDocument> doc(document(source));
        // A document out of batch mode would show the format options dialog
        // for every export; saved exports must not.
        doc->setFileBatchMode(false);
        doc->setModified(false);

        Saved::Preset same = Saved::newPreset("Same");
        Saved::Preset game = Saved::newPreset("Game");
        game.format = "jpg";
        game.destination = Saved::Destination::Folder;
        game.folderRelative = true;
        game.folder = "../game/gfx";
        game.nameRule = Saved::NameRule::Fixed;
        game.fixedName = "hero_web";
        Saved::Preset crop = Saved::newPreset("Crop");
        crop.area = Saved::Area::Selection;
        crop.nameRule = Saved::NameRule::Fixed;
        crop.fixedName = "crop";
        crop.enabled = false;
        doc->setSolsticeFastExportData(Saved::toXml({same, game, crop}));
        const int undoCount = doc->undoStack()->count();

        bool dialogSeen = false;
        QTimer guard;
        connect(&guard, &QTimer::timeout, &guard, [&] {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                dialogSeen = true;
                modal->close();
            }
        });
        guard.start(100);
        auto wait = [](KisImageSP image) {
            image->waitForDone();
            return true;
        };

        // Checked: the crop is skipped without being reported.
        auto result = Saved::run({doc.data()}, Saved::Which::Checked, wait);
        QCOMPARE(result.exported, 2);
        QVERIFY(result.skipped.isEmpty());
        QVERIFY(result.conflicts.isEmpty());
        QCOMPARE(QImage(dir.filePath("art/hero.png")).size(), QSize(16, 12));
        QCOMPARE(QImage(dir.filePath("game/gfx/hero_web.jpg")).size(), QSize(16, 12));
        QVERIFY(!QFileInfo::exists(dir.filePath("art/crop.png")));

        // All: without a selection only the crop is skipped.
        QVERIFY(QFile::remove(dir.filePath("art/hero.png")));
        result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 2);
        QCOMPARE(result.skipped, QStringList({doc->caption() + " / Crop: no selection"}));
        QVERIFY(QFileInfo::exists(dir.filePath("art/hero.png")));

        selectRect(doc.data(), QRect(2, 3, 5, 4));
        doc->setModified(false);
        const int selectedUndoCount = doc->undoStack()->count();
        result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 3);
        QVERIFY(result.skipped.isEmpty());
        QImage cropped(dir.filePath("art/crop.png"));
        QCOMPARE(cropped.size(), QSize(5, 4));
        QCOMPARE(cropped.pixelColor(0, 0).alpha(), 127);

        QVERIFY(!dialogSeen);
        QCOMPARE(doc->localFilePath(), source);
        QVERIFY(!doc->isModified());
        QVERIFY(!doc->fileBatchMode());
        QCOMPARE(doc->undoStack()->count(), selectedUndoCount);
        QVERIFY(selectedUndoCount > undoCount);
        QCOMPARE(doc->image()->size(), QSize(16, 12));

        // Two presets writing one file: nothing is written.
        Saved::Preset twin = same;
        twin.id = Saved::newPreset({}).id;
        twin.name = "Twin";
        doc->setSolsticeFastExportData(Saved::toXml({same, twin, game}));
        QVERIFY(QFile::remove(dir.filePath("game/gfx/hero_web.jpg")));
        result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 0);
        QCOMPARE(result.conflicts, QStringList({QDir::toNativeSeparators(dir.filePath("art/hero.png"))}));
        QVERIFY(!QFileInfo::exists(dir.filePath("game/gfx/hero_web.jpg")));

        // Documents that cannot run, a missing folder and an invalid name.
        Saved::Preset missing = Saved::newPreset("Missing");
        missing.destination = Saved::Destination::Folder;
        missing.folder = dir.filePath("does/not/exist");
        Saved::Preset invalid = Saved::newPreset("Invalid");
        invalid.nameRule = Saved::NameRule::Fixed;
        invalid.fixedName = "a:b";
        Saved::Preset noFolder = Saved::newPreset("No folder");
        noFolder.destination = Saved::Destination::Folder;
        doc->setSolsticeFastExportData(Saved::toXml({missing, invalid, noFolder}));
        QScopedPointer<KisDocument> unsaved(document({}));
        unsaved->setSolsticeFastExportData(Saved::toXml({same}));
        QScopedPointer<KisDocument> bare(document(dir.filePath("bare.kra")));
        QScopedPointer<KisDocument> unchecked(document(dir.filePath("unchecked.kra")));
        unchecked->setSolsticeFastExportData(Saved::toXml({crop}));
        result = Saved::run({nullptr, unsaved.data(), bare.data(), unchecked.data(), doc.data()},
                            Saved::Which::Checked,
                            wait);
        QCOMPARE(result.exported, 0);
        QCOMPARE(result.skipped,
                 QStringList({"Unknown document: not available",
                              unsaved->caption() + ": not saved yet",
                              bare->caption() + ": no saved exports",
                              unchecked->caption() + ": no checked exports",
                              doc->caption() + " / Invalid: invalid file name",
                              doc->caption() + " / No folder: no export folder",
                              doc->caption() + " / Missing: could not export"}));
        QVERIFY(!dialogSeen);
    }

    void presetRunLayerComps()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("comps.kra");
        QScopedPointer<KisDocument> doc(document(source));
        doc->setFileBatchMode(false);
        const auto comps = addLayerComps(doc.data());
        KisImageSP image = doc->image();
        KisNodeSP top = image->root()->lastChild();
        QCOMPARE(top->name(), QString("top"));

        const auto listed = Saved::layerComps(doc.data());
        QCOMPARE(listed.size(), 2);
        QCOMPARE(listed[0].name, QString("Base"));
        QCOMPARE(listed[1].id, comps[1]->uuid().toString(QUuid::WithoutBraces));

        Saved::Preset base = layerCompPreset("Base PNG", comps[0]);
        Saved::Preset web = layerCompPreset("Top JPEG", comps[1]);
        web.format = "jpg";
        Saved::Preset plain = Saved::newPreset("Plain");
        // The selection is made after the comps, which therefore hide its mask.
        Saved::Preset crop = layerCompPreset("Crop", comps[0]);
        crop.area = Saved::Area::Selection;
        crop.nameRule = Saved::NameRule::Fixed;
        crop.fixedName = "crop";
        Saved::Preset gone = Saved::newPreset("Gone");
        gone.layerComp = QUuid::createUuid().toString(QUuid::WithoutBraces);
        gone.layerCompName = "Deleted";
        doc->setSolsticeFastExportData(Saved::toXml({base, web, plain, crop, gone}));
        selectRect(doc.data(), QRect(2, 3, 5, 4));
        doc->setModified(false);
        const int undoCount = doc->undoStack()->count();

        bool dialogSeen = false;
        QTimer guard;
        connect(&guard, &QTimer::timeout, &guard, [&] {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                dialogSeen = true;
                modal->close();
            }
        });
        guard.start(100);
        auto wait = [](KisImageSP image) {
            image->waitForDone();
            return true;
        };

        auto result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 4);
        QCOMPARE(result.skipped, QStringList({doc->caption() + " / Gone: layer comp not found"}));
        QVERIFY(!dialogSeen);
        // Base hides the red layer, Top and the layers as they are show it.
        const QColor baseColor = QImage(dir.filePath("comps_Base.png")).pixelColor(0, 0);
        QCOMPARE(baseColor.alpha(), 127);
        QVERIFY(baseColor.red() < 120 && baseColor.blue() > 160);
        const QColor plainColor = QImage(dir.filePath("comps.png")).pixelColor(0, 0);
        QCOMPARE(plainColor, QColor(255, 0, 0));
        const QColor webColor = QImage(dir.filePath("comps_Top.jpg")).pixelColor(0, 0);
        QVERIFY(webColor.red() > 220 && webColor.green() < 40 && webColor.blue() < 40);
        const QImage cropped(dir.filePath("crop.png"));
        QCOMPARE(cropped.size(), QSize(5, 4));
        QCOMPARE(cropped.pixelColor(0, 0).alpha(), 127);

        // The document is as it was: no comp applied, nothing to undo.
        QVERIFY(top->visible());
        QVERIFY(!doc->isModified());
        QCOMPARE(doc->undoStack()->count(), undoCount);
        QVERIFY(!doc->fileBatchMode());
        QVERIFY(image->globalSelection());
        QColor live;
        image->projection()->pixel(0, 0, &live);
        QCOMPARE(live, QColor(255, 0, 0));

        // A renamed comp is found by its id and names the file after its new name.
        comps[1]->setName("Renamed");
        QCOMPARE(Saved::findLayerComp(image, web), comps[1]);
        doc->setSolsticeFastExportData(Saved::toXml({web}));
        result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 1);
        QVERIFY(QFileInfo::exists(dir.filePath("comps_Renamed.jpg")));

        // Without its id (a file saved by Krita), the only comp with its name.
        Saved::Preset byName = web;
        byName.layerComp = QUuid::createUuid().toString(QUuid::WithoutBraces);
        byName.layerCompName = "Renamed";
        QCOMPARE(Saved::findLayerComp(image, byName), comps[1]);
        comps[0]->setName("Renamed");
        QVERIFY(!Saved::findLayerComp(image, byName));
        QVERIFY(!Saved::findLayerComp(image, plain));

        // Two comps writing one file: nothing is written.
        Saved::Preset twin = layerCompPreset("Twin", comps[0]);
        twin.nameRule = Saved::NameRule::Fixed;
        twin.fixedName = "same";
        Saved::Preset twin2 = layerCompPreset("Twin 2", comps[1]);
        twin2.nameRule = Saved::NameRule::Fixed;
        twin2.fixedName = "same";
        doc->setSolsticeFastExportData(Saved::toXml({twin, twin2}));
        result = Saved::run({doc.data()}, Saved::Which::All, wait);
        QCOMPARE(result.exported, 0);
        QCOMPARE(result.conflicts, QStringList({QDir::toNativeSeparators(dir.filePath("same.png"))}));
        QVERIFY(!dialogSeen);
    }

    void presetDialogLayerComps()
    {
        QTemporaryDir dir;
        QScopedPointer<KisDocument> doc(document(dir.filePath("dialog.kra")));
        const auto comps = addLayerComps(doc.data());
        saveSameFolderSettings();
        QWidget parent;
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(
            &parent,
            [&](bool) {
                return QList<QPointer<KisDocument>>({doc.data()});
            },
            [](KisImageSP image) {
                image->waitForDone();
                return true;
            },
            doc.data());
        auto *list = dialog->findChild<QListWidget *>();
        auto *layerComp = dialog->findChildren<QComboBox *>()[5];
        QLineEdit *name = nullptr;
        for (auto *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == "Same as the document")
                name = edit;
        }
        QVERIFY(list && name);
        QVERIFY(!layerComp->isEnabled());
        QCOMPARE(layerComp->count(), 3);
        QCOMPARE(layerComp->itemText(1), QString("Base"));

        button(dialog, "Add")->click();
        QVERIFY(layerComp->isEnabled());
        QCOMPARE(layerComp->currentIndex(), 0);
        QVERIFY(Saved::presets(doc.data())[0].layerComp.isEmpty());
        layerComp->setCurrentIndex(2);
        auto preset = Saved::presets(doc.data())[0];
        QCOMPARE(preset.layerComp, comps[1]->uuid().toString(QUuid::WithoutBraces));
        QCOMPARE(preset.layerCompName, QString("Top"));
        QCOMPARE(name->placeholderText(), QString("Document name_layer comp name"));
        QVERIFY(list->item(1)->toolTip().contains("Layer comp Top"));

        // A deleted comp stays selected as missing, and other edits keep it.
        doc->image()->removeComposition(comps[1]);
        list->setCurrentRow(0);
        QCOMPARE(layerComp->count(), 2);
        QCOMPARE(name->placeholderText(), QString("Same as the document"));
        list->setCurrentRow(1);
        QCOMPARE(layerComp->count(), 3);
        QCOMPARE(layerComp->currentText(), QString("Top (missing)"));
        dialog->findChildren<QSpinBox *>()[0]->setValue(2);
        preset = Saved::presets(doc.data())[0];
        QCOMPARE(preset.pngCompression, 2);
        QCOMPARE(preset.layerComp, comps[1]->uuid().toString(QUuid::WithoutBraces));
        QCOMPARE(preset.layerCompName, QString("Top"));

        layerComp->setCurrentIndex(0);
        QVERIFY(Saved::presets(doc.data())[0].layerComp.isEmpty());
        delete dialog;
    }

    void presetDialog()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("dialog.kra");
        QScopedPointer<KisDocument> doc(document(source));
        doc->setModified(false);
        saveSameFolderSettings();
        QWidget parent;
        QPointer<QDialog> dialog = KisLazyToolsFastExportDialog::create(
            &parent,
            [&](bool) {
                return QList<QPointer<KisDocument>>({doc.data()});
            },
            [](KisImageSP image) {
                image->waitForDone();
                return true;
            },
            doc.data());
        auto *list = dialog->findChild<QListWidget *>();
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        auto *exportButton = button(dialog, "Export");
        auto *exportAll = button(dialog, "Export All");
        auto *exportChecked = button(dialog, "Export Checked");
        QVERIFY(exportButton && exportAll && exportChecked);
        QVERIFY(exportButton->isEnabled());
        QVERIFY(!exportAll->isEnabled());
        const auto combos = dialog->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 6);
        auto *format = combos[0];
        auto *area = combos[4];
        auto *folder = dialog->findChild<QLineEdit *>();
        QLineEdit *name = nullptr;
        for (auto *edit : dialog->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == "Same as the document")
                name = edit;
        }
        QVERIFY(name);
        QVERIFY(!area->isEnabled());

        // The one-off form becomes the new saved export.
        format->setCurrentIndex(1);
        name->setText("first");
        button(dialog, "Add")->click();
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentRow(), 1);
        QTRY_VERIFY(doc->isModified());
        auto presets = Saved::presets(doc.data());
        QCOMPARE(presets.size(), 1);
        QCOMPARE(presets[0].name, QString("Export 1"));
        QCOMPARE(presets[0].format, QString("jpg"));
        QCOMPARE(presets[0].fixedName, QString("first"));
        QVERIFY(presets[0].enabled);
        QVERIFY(!exportButton->isEnabled());
        QVERIFY(area->isEnabled());
        QVERIFY(exportAll->isEnabled());

        // Editing the form changes the selected saved export.
        format->setCurrentIndex(0);
        area->setCurrentIndex(1);
        name->setText("bad/name");
        presets = Saved::presets(doc.data());
        QCOMPARE(presets[0].format, QString("png"));
        QCOMPARE(presets[0].area, Saved::Area::Selection);
        QCOMPARE(presets[0].fixedName, QString("first"));
        name->clear();
        area->setCurrentIndex(0);
        presets = Saved::presets(doc.data());
        QCOMPARE(presets[0].nameRule, Saved::NameRule::DocumentName);
        QCOMPARE(presets[0].area, Saved::Area::WholeImage);

        // Rename and uncheck in the list.
        list->item(1)->setText("Godot");
        list->item(1)->setCheckState(Qt::Unchecked);
        presets = Saved::presets(doc.data());
        QCOMPARE(presets[0].name, QString("Godot"));
        QVERIFY(!presets[0].enabled);

        button(dialog, "Duplicate")->click();
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->currentRow(), 2);
        presets = Saved::presets(doc.data());
        QCOMPARE(presets[1].name, QString("Godot copy"));
        QVERIFY(presets[1].id != presets[0].id);
        format->setCurrentIndex(1);
        name->setText("copy");
        QVERIFY(button(dialog, "Move Down") && !button(dialog, "Move Down")->isEnabled());
        button(dialog, "Move Up")->click();
        presets = Saved::presets(doc.data());
        QCOMPARE(presets[0].name, QString("Godot copy"));
        QCOMPARE(presets[0].format, QString("jpg"));
        QCOMPARE(list->currentRow(), 1);

        // Back to the one-off row: the one-off settings return.
        list->setCurrentRow(0);
        QCOMPARE(format->currentIndex(), 1);
        QCOMPARE(name->text(), QString("first"));
        QVERIFY(exportButton->isEnabled());
        QVERIFY(!area->isEnabled());

        list->setCurrentRow(2);
        button(dialog, "Remove")->click();
        QCOMPARE(Saved::presets(doc.data()).size(), 1);
        QCOMPARE(list->count(), 2);
        // The duplicate kept the original's unchecked state.
        list->item(1)->setCheckState(Qt::Checked);

        // Export Checked runs only the checked saved export and closes.
        exportChecked->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(QImage(dir.filePath("copy.jpg")).size(), QSize(16, 12));
        QVERIFY(!QFileInfo::exists(dir.filePath("dialog.png")));
        QCOMPARE(doc->localFilePath(), source);
        Q_UNUSED(folder);

        // Without a document the list cannot be used.
        QPointer<QDialog> noDocument = KisLazyToolsFastExportDialog::create(
            &parent,
            [](bool) {
                return QList<QPointer<KisDocument>>();
            },
            [](KisImageSP) {
                return true;
            });
        QVERIFY(!noDocument->findChild<QListWidget *>()->isEnabled());
        QVERIFY(!button(noDocument, "Add")->isEnabled());
        delete noDocument;
    }
};

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir profile;
    if (!profile.isValid())
        return 1;
    qputenv("SOLSTICE_PROFILE_ROOT", profile.path().toUtf8());
    qputenv("LANGUAGE", "en");
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));
    qputenv("KRITA_PLUGIN_PATH", QByteArray(KRITA_PLUGINS_DIR_FOR_TESTS));
    kisTestSetupFontconfig();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    KConfig::setMainConfigName(profile.filePath("test-kritarc"));
    KLocalizedString::setApplicationDomain("krita");
    registerResources();
    KisLazyToolsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "KisLazyToolsTest.moc"
