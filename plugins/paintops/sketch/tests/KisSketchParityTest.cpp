/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include <KisStandardOptionData.h>

#include "../KisSketchOpOptionData.h"
#include "../KisSketchStandardOptionData.h"
#include "../kis_sketch_paintop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Sketch editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles; each is also
 * tried with a color tip of the RGBA brushes bundle in lightness map mode,
 * which Sketch does not support.
 */
class KisSketchParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("sketch"));
}
} // namespace

void KisSketchParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 13);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("colortip")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisSketchParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    }

    KisSketchPaintOpSettingsWidget widget(nullptr);
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisSketchParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Sketch_fur.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisSketchPaintOpSettingsWidget widget(nullptr);
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    // the first edit rewrites everything
    auto *sketch = typedOption<KisSketchOpOptionData>(model, QStringLiteral("Sketch"));
    QVERIFY(sketch);
    KisSketchOpOptionData sketchData = sketch->data();
    sketchData.offset = 42.0;
    sketch->cursor().set(sketchData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    QCOMPARE(settings->getDouble("Sketch/offset"), 42.0);

    auto *lineWidth = typedOption<KisLineWidthOptionData>(model, QStringLiteral("LineWidth"));
    QVERIFY(lineWidth);
    KisLineWidthOptionData lineWidthData = lineWidth->data();
    lineWidthData.isChecked = !lineWidthData.isChecked;
    lineWidth->cursor().set(lineWidthData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

void KisSketchParityTest::testToolOptionsIds()
{
    KisSketchPaintOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("sketchbrush"),
                         {QStringLiteral("BrushTip"),
                          QStringLiteral("Sketch"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Size"),
                          QStringLiteral("LineWidth"),
                          QStringLiteral("OffsetScale"),
                          QStringLiteral("Density"),
                          QStringLiteral("PaintingMode")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisSketchParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisSketchParityTest.moc"
