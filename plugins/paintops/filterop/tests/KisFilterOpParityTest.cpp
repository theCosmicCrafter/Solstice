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

#include <KisFilterOptionData.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_registry.h>

#include "../kis_filterop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Filter editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles.
 * Each is also tried with a color tip of the RGBA brushes bundle in
 * lightness map mode.
 */
class KisFilterOpParityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testLegacyRewrite_data();
    void testLegacyRewrite();
    void testModelEditsKeepPresetConsistent();
    void testToolOptionsIds();
    void testFallbackFilter();
};

namespace
{
QString dataDir()
{
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("filterop"));
}
} // namespace

void KisFilterOpParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 3);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("colortip")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisFilterOpParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    }

    KisFilterOpSettingsWidget widget(nullptr);
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisFilterOpParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("FX_blur_light.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisFilterOpSettingsWidget widget(nullptr);
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

void KisFilterOpParityTest::testToolOptionsIds()
{
    KisFilterOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("filter"),
                         {QStringLiteral("BrushTip"),
                          QStringLiteral("CompositeOp"),
                          QStringLiteral("Opacity"),
                          QStringLiteral("Size"),
                          QStringLiteral("Rotation"),
                          QStringLiteral("Filter")});
}

/// A preset without a filter is written with the fallback filter, as the
/// option's page did before the migration.
void KisFilterOpParityTest::testFallbackFilter()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("FX_blur_light.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();
    settings->removeProperty("Filter/id");
    settings->removeProperty("Filter/configuration");

    KisFilterOpSettingsWidget widget(nullptr);
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    auto *filter = typedOption<KisFilterOptionData>(model, QStringLiteral("Filter"));
    QVERIFY(filter);
    QVERIFY(filter->data().filterId.isEmpty());
    KisFilterOptionData filterData = filter->data();
    filterData.smudgeMode = !filterData.smudgeMode;
    filter->cursor().set(filterData);

    const QString fallbackId = KisFilterRegistry::instance()->fallbackFilter()->id();
    QCOMPARE(settings->getString("Filter/id"), fallbackId);
    QVERIFY(!settings->getString("Filter/configuration").isEmpty());
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisFilterOpParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisFilterOpParityTest.moc"
