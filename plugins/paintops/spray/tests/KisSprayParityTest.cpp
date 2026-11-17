/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include <KisCompositeOpOptionData.h>
#include <KisStandardOptionData.h>

#include "../KisSprayOpOptionData.h"
#include "../KisSprayShapeOptionData.h"

#include "../kis_spray_paintop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Spray editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles.
 * Each is also tried with a color tip of the RGBA brushes bundle in
 * lightness map mode.
 */
class KisSprayParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("spray"));
}
} // namespace

void KisSprayParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 5);
    for (const QString &file : files) {
        for (const QString &variant :
             {QStringLiteral("base"), QStringLiteral("colortip"), QStringLiteral("proportional")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisSprayParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    } else if (variant == QStringLiteral("proportional")) {
        // a particle size relative to the spray area's diameter and scale
        preset->settings()->setProperty("SprayShape/enabled", true);
        preset->settings()->setProperty("SprayShape/proportional", true);
        preset->settings()->setProperty("SprayShape/width", 40);
        preset->settings()->setProperty("SprayShape/height", 25);
    }

    KisSprayPaintOpSettingsWidget widget(nullptr);
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisSprayParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Spray_splat.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisSprayPaintOpSettingsWidget widget(nullptr);
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

    auto *engine = typedOption<KisSprayOpOptionData>(model, QStringLiteral("SprayOp"));
    QVERIFY(engine);
    KisSprayOpOptionData data = engine->data();
    data.diameter = data.diameter + 20;
    engine->cursor().set(data);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));

    // the shape page reads the spray area's diameter and scale through its
    // own cursors; toggling proportional sizes writes only the shape
    auto *shape = typedOption<KisSprayShapeOptionData>(model, QStringLiteral("SprayShape"));
    QVERIFY(shape);
    KisSprayShapeOptionData shapeData = shape->data();
    shapeData.proportional = !shapeData.proportional;
    shape->cursor().set(shapeData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    QCOMPARE(settings->getBool("SprayShape/proportional"), shapeData.proportional);
}

void KisSprayParityTest::testToolOptionsIds()
{
    KisSprayPaintOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("spraybrush"),
                         {QStringLiteral("SprayOp"),
                          QStringLiteral("SprayShape"),
                          QStringLiteral("BrushTip"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Size"),
                          QStringLiteral("CompositeOp"),
                          QStringLiteral("ShapeDynamics"),
                          QStringLiteral("ColorOptions"),
                          QStringLiteral("Rotation"),
                          QStringLiteral("Airbrush"),
                          QStringLiteral("Rate"),
                          QStringLiteral("PaintingMode")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisSprayParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisSprayParityTest.moc"
