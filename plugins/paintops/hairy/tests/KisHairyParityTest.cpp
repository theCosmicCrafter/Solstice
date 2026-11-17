/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include <KisStandardOptionData.h>

#include "../KisHairyBristleOptionData.h"
#include "../KisHairyInkOptionData.h"
#include "../kis_hairy_paintop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Bristle (Hairy) editor
 * must write the same keys and values for every preset as before its
 * migration to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles; each is also
 * tried with a color tip of the RGBA brushes bundle in lightness map mode,
 * which Bristle does not support.
 */
class KisHairyParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("hairy"));
}
} // namespace

void KisHairyParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 7);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("colortip")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisHairyParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    }

    KisHairyPaintOpSettingsWidget widget(nullptr);
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisHairyParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Bristle_Texture.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisHairyPaintOpSettingsWidget widget(nullptr);
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    // the first edit rewrites everything
    auto *bristle = typedOption<KisHairyBristleOptionData>(model, QStringLiteral("Bristle"));
    QVERIFY(bristle);
    KisHairyBristleOptionData bristleData = bristle->data();
    bristleData.scaleFactor = 3.5;
    bristle->cursor().set(bristleData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));

    auto *ink = typedOption<KisHairyInkOptionData>(model, QStringLiteral("Ink"));
    QVERIFY(ink);
    KisHairyInkOptionData inkData = ink->data();
    inkData.inkAmount = 512;
    inkData.inkDepletionEnabled = !inkData.inkDepletionEnabled;
    ink->cursor().set(inkData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

/// Bristle options can be shown in Tool Options; the tip settings that the
/// Bristle editor hides (the auto tip's fade, density and spacing) have no
/// eyes.
void KisHairyParityTest::testToolOptionsIds()
{
    KisHairyPaintOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("hairybrush"),
                         {QStringLiteral("BrushTip"),
                          QStringLiteral("Bristle"),
                          QStringLiteral("Ink"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Size"),
                          QStringLiteral("PaintingMode")});

    Q_FOREACH (KisPaintOpOption *option, widget.toolOptionsOptions()) {
        if (option->toolOptionsId() != QStringLiteral("BrushTip")) {
            continue;
        }
        QStringList parameters;
        Q_FOREACH (const KisPaintOpOption::ToolOptionsParameter &parameter, option->toolOptionsParameters()) {
            parameters << parameter.id;
        }
        QVERIFY(parameters.contains(QStringLiteral("Diameter")));
        // the predefined tip's spacing stays visible
        QVERIFY(parameters.contains(QStringLiteral("PredefinedSpacing")));
        for (const QString &hidden : {QStringLiteral("Fade"), QStringLiteral("Density"), QStringLiteral("Spacing")}) {
            QVERIFY2(!parameters.contains(hidden), qPrintable(hidden));
        }
    }
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisHairyParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisHairyParityTest.moc"
