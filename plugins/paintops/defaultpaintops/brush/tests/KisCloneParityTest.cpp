/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "KisBrushTestMain.h"
#include "KisPaintOpParityTestUtils.h"

#include <KisCompositeOpOptionData.h>
#include <KisSizeOptionData.h>
#include <KisStandardOptionData.h>

#include "../../duplicate/KisDuplicateOptionData.h"
#include "../../duplicate/kis_duplicateop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Clone editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles; each is also
 * tried with a color tip of the RGBA brushes bundle in lightness map mode,
 * and with every clone option on.
 */
class KisCloneParityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testLegacyRewrite_data();
    void testLegacyRewrite();
    void testModelEditsKeepPresetConsistent();
    void testToolOptionsIds();
};

namespace
{
QString dataDir()
{
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("clone"));
}

struct Editor {
    Editor()
        : widget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP())
    {
    }
    KisDuplicateOpSettingsWidget widget;
};
} // namespace

void KisCloneParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 2);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("colortip"), QStringLiteral("options")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisCloneParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    } else if (variant == QStringLiteral("options")) {
        for (const QString &key : {DUPLICATE_HEALING,
                                   DUPLICATE_CORRECT_PERSPECTIVE,
                                   DUPLICATE_MOVE_SOURCE_POINT,
                                   DUPLICATE_RESET_SOURCE_POINT,
                                   DUPLICATE_CLONE_FROM_PROJECTION}) {
            preset->settings()->setProperty(key, true);
        }
    }

    Editor editor;
    compareWithReference(
        fullRewrite(&editor.widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisCloneParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Clone_tool.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    Editor editor;
    editor.widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = editor.widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    // the first edit rewrites everything
    auto *compositeOp = typedOption<KisCompositeOpOptionData>(model, QStringLiteral("CompositeOp"));
    QVERIFY(compositeOp);
    KisCompositeOpOptionData compositeOpData = compositeOp->data();
    compositeOpData.compositeOpId = QStringLiteral("multiply");
    compositeOp->cursor().set(compositeOpData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    QCOMPARE(settings->getString("CompositeOp"), QStringLiteral("multiply"));

    auto *clone = typedOption<KisDuplicateOptionData>(model, QStringLiteral("Clone"));
    QVERIFY(clone);
    KisDuplicateOptionData cloneData = clone->data();
    cloneData.healing = !cloneData.healing;
    clone->cursor().set(cloneData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    QCOMPARE(settings->getBool(DUPLICATE_HEALING), cloneData.healing);

    auto *size = typedOption<KisSizeOptionData>(model, QStringLiteral("Size"));
    QVERIFY(size);
    KisSizeOptionData sizeData = size->data();
    sizeData.isChecked = !sizeData.isChecked;
    size->cursor().set(sizeData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

void KisCloneParityTest::testToolOptionsIds()
{
    Editor editor;
    verifyToolOptionsIds(&editor.widget,
                         QStringLiteral("duplicate"),
                         {QStringLiteral("BrushTip"),
                          QStringLiteral("CompositeOp"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Size"),
                          QStringLiteral("Clone"),
                          QStringLiteral("Texture")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisCloneParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisCloneParityTest.moc"
