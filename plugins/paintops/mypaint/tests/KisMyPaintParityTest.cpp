/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include "../MyPaintBasicOptionData.h"
#include "../MyPaintCurveOptionData.h"
#include "../MyPaintPaintOpPreset.h"
#include "../MyPaintPaintOpSettingsWidget.h"
#include "../MyPaintStandardOptionData.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the MyPaint editor must
 * write the same keys and values for every brush as before its migration to
 * the options model.
 *
 * The brushes are the bundled MyPaint brushes (plugins/paintops/mypaint/
 * brushes). Every curve option patches its own part of the shared
 * MyPaint/json document, so the references include that document.
 */
class KisMyPaintParityTest : public QObject
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
const QString mypaintJson = QStringLiteral("MyPaint/json");

QString dataDir()
{
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("mypaint"));
}

KisPaintOpPresetSP loadBrush(const QString &fileName)
{
    KisPaintOpPresetSP preset(new KisMyPaintPaintOpPreset(QDir(dataDir()).filePath(fileName)));
    return preset->load(KisGlobalResourcesInterface::instance()) && preset->settings() ? preset : KisPaintOpPresetSP();
}

/// A full write of the model onto the preset's MyPaint/json document, which
/// the options patch (KisMyPaintOpSettings::resetSettings() keeps it too)
QStringList fullModelWriteOnDocument(KisPaintOpOptionsModel *model, KisPaintOpSettingsSP settings)
{
    KisPropertiesConfigurationSP config(new KisPropertiesConfiguration());
    config->setProperty(mypaintJson, settings->getProperty(mypaintJson));
    Q_FOREACH (KisPaintOpOptionStateBase *option, model->options()) {
        option->write(config.data());
    }
    return serialize(config->getProperties());
}
} // namespace

void KisMyPaintParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.myb")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 7);
    for (const QString &file : files) {
        QTest::newRow(qPrintable(file)) << file;
    }
}

void KisMyPaintParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    KisPaintOpPresetSP preset = loadBrush(fileName);
    QVERIFY(preset);

    KisMyPaintOpSettingsWidget widget(nullptr);
    compareWithReference(fullRewrite(&widget, preset),
                         QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes:
/// the Basic page, a curve option's base value (in the shared document) and
/// the Basic page's base values, which edit the curve options.
void KisMyPaintParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadBrush(QStringLiteral("i_Wet_Paint_Plus_mypaint.myb"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisMyPaintOpSettingsWidget widget(nullptr);
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    // the first edit rewrites everything
    auto *basic = typedOption<MyPaintBasicOptionData>(model, QStringLiteral("Basic"));
    QVERIFY(basic);
    MyPaintBasicOptionData basicData = basic->data();
    basicData.eraserMode = !basicData.eraserMode;
    basic->cursor().set(basicData);
    QCOMPARE(presetOptionProperties(settings), fullModelWriteOnDocument(model, settings));
    QCOMPARE(settings->getBool("EraserMode"), basicData.eraserMode);

    auto *smudge = typedOption<MyPaintSmudgeData>(model, QStringLiteral("smudge"));
    QVERIFY(smudge);
    MyPaintSmudgeData smudgeData = smudge->data();
    smudgeData.strengthValue = 0.25;
    smudge->cursor().set(smudgeData);
    QCOMPARE(presetOptionProperties(settings), fullModelWriteOnDocument(model, settings));
    QVERIFY(settings->getString(mypaintJson).contains(QStringLiteral("\"smudge\"")));

    auto *radius = typedOption<MyPaintRadiusLogarithmicData>(model, QStringLiteral("radius_logarithmic"));
    QVERIFY(radius);
    MyPaintRadiusLogarithmicData radiusData = radius->data();
    radiusData.strengthValue = 1.5;
    radius->cursor().set(radiusData);
    QCOMPARE(presetOptionProperties(settings), fullModelWriteOnDocument(model, settings));
    QCOMPARE(settings->getDouble("MyPaint/diameter"), 2.0 * exp(1.5));
}

void KisMyPaintParityTest::testToolOptionsIds()
{
    KisMyPaintOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("mypaintbrush"),
                         {QStringLiteral("Basic"),
                          QStringLiteral("radius_logarithmic"),
                          QStringLiteral("hardness"),
                          QStringLiteral("opaque"),
                          QStringLiteral("smudge"),
                          QStringLiteral("Airbrush")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisMyPaintParityTest, QStringLiteral("Krita_4_Default_Resources.bundle"))

#include "KisMyPaintParityTest.moc"
