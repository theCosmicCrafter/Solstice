/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QTest>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"
#include "../../defaultpaintops/brush/tests/KisPaintOpParityTestUtils.h"

#include <KisCompositeOpOptionData.h>
#include <KisStandardOptionData.h>

#include "../KisParticleOpOptionData.h"

#include "../kis_particle_paintop_settings_widget.h"

using namespace KisPaintOpParityTestUtils;

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Particle editor must
 * write the same keys and values for every preset as before its migration
 * to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles.
 */
class KisParticleParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("particle"));
}
} // namespace

void KisParticleParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 2);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisParticleParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(dataDir(), fileName);
    QVERIFY(preset);
    if (variant == QStringLiteral("colortip")) {
        useColorTip(preset->settings());
    }

    KisParticlePaintOpSettingsWidget widget(nullptr);
    compareWithReference(
        fullRewrite(&widget, preset),
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties"));
}

/// After every edit the preset holds what a full write of the model writes.
void KisParticleParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(dataDir(), QStringLiteral("Special_particles_crazy.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisParticlePaintOpSettingsWidget widget(nullptr);
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

    auto *engine = typedOption<KisParticleOpOptionData>(model, QStringLiteral("ParticleOp"));
    QVERIFY(engine);
    KisParticleOpOptionData data = engine->data();
    data.particleCount = data.particleCount + 20;
    engine->cursor().set(data);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

void KisParticleParityTest::testToolOptionsIds()
{
    KisParticlePaintOpSettingsWidget widget(nullptr);
    verifyToolOptionsIds(&widget,
                         QStringLiteral("particlebrush"),
                         {QStringLiteral("ParticleOp"),
                          QStringLiteral("CompositeOp"),
                          QStringLiteral("Airbrush"),
                          QStringLiteral("Rate"),
                          QStringLiteral("PaintingMode")});
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisParticleParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisParticleParityTest.moc"
