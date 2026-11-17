/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include <KisCompositeOpOptionData.h>
#include <KisSizeOptionData.h>
#include <KisStandardOptionData.h>

#include "../kis_tangent_normal_paintop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Tangent Normal editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles.
 * Each is also tried with a color tip of the RGBA brushes bundle in
 * lightness map mode.
 */
class KisTangentNormalParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("tangentnormal"));
}
} // namespace

void KisTangentNormalParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 6);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("colortip")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisTangentNormalParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    }

    KisTangentNormalPaintOpSettingsWidget widget(nullptr,
                                                 KisGlobalResourcesInterface::instance(),
                                                 KoCanvasResourcesInterfaceSP());
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisTangentNormalParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Tangent_normal_basic.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisTangentNormalPaintOpSettingsWidget widget(nullptr,
                                                 KisGlobalResourcesInterface::instance(),
                                                 KoCanvasResourcesInterfaceSP());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
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

    auto *size = typedOption<KisSizeOptionData>(model, QStringLiteral("Size"));
    QVERIFY(size);
    KisSizeOptionData sizeData = size->data();
    sizeData.isChecked = !sizeData.isChecked;
    size->cursor().set(sizeData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

void KisTangentNormalParityTest::testToolOptionsIds()
{
    KisTangentNormalPaintOpSettingsWidget widget(nullptr,
                                                 KisGlobalResourcesInterface::instance(),
                                                 KoCanvasResourcesInterfaceSP());
    verifyToolOptionsIds(&widget,
                         QStringLiteral("tangentnormal"),
                         {QStringLiteral("BrushTip"),
                          QStringLiteral("CompositeOp"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Flow"),
                          QStringLiteral("Size"),
                          QStringLiteral("TangentTilt"),
                          QStringLiteral("Spacing"),
                          QStringLiteral("Texture"),
                          QStringLiteral("PaintingMode")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisTangentNormalParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisTangentNormalParityTest.moc"
