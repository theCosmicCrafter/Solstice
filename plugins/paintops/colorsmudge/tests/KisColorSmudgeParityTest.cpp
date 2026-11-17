/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>
#include <QToolButton>

#include "../../defaultpaintops/brush/tests/KisBrushTestMain.h"

#include "../KisSmudgeLengthOptionData.h"
#include <KisBrushBasedOptionStates.h>
#include <KisBrushModel.h>
#include <KisGlobalResourcesInterface.h>
#include <KisPaintOpOptionsModel.h>
#include <KisStandardOptionData.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_brush.h>
#include <kis_paintop_option.h>
#include <kis_predefined_brush_factory.h>

#include "../kis_colorsmudgeop_settings_widget.h"

/**
 * Phase 4 of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the Color Smudge editor
 * must write the same keys and values for every preset as before its
 * migration to the options model.
 *
 * The presets come from the Krita 3 and 4 default bundles; each is also
 * tried with the new smudge engine, overlay mode and paint thickness on (and
 * a smudge radius above the new engine's range), and with a color tip of the
 * RGBA brushes bundle in lightness map mode, which the old presets do not use.
 *
 * The references in the .properties files in data/colorsmudge were written
 * by the code before the migration (SOLSTICE_WRITE_REFERENCES=1 writes them
 * again).
 */
class KisColorSmudgeParityTest : public QObject
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
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("colorsmudge"));
}

KisPaintOpPresetSP loadPreset(const QString &fileName)
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset(QDir(dataDir()).filePath(fileName)));
    return preset->load(KisGlobalResourcesInterface::instance()) && preset->settings() ? preset : KisPaintOpPresetSP();
}

void applyVariant(KisPaintOpSettingsSP settings, const QString &variant)
{
    if (variant == QStringLiteral("new")) {
        settings->setProperty("SmudgeRateUseNewEngine", true);
        settings->setProperty("MergedPaint", true);
        settings->setProperty("PressurePaintThickness", true);
        settings->setProperty("PaintThicknessThicknessMode", 1);
        // above the new engine's range (version 2 stores the value as is)
        settings->setProperty("SmudgeRadiusVersion", 2);
        settings->setProperty("SmudgeRadiusValue", 2.5);
    } else if (variant == QStringLiteral("lightness")) {
        // a color tip in lightness map mode
        KisResourcesInterfaceSP resources = KisGlobalResourcesInterface::instance();
        const KisBrushSP tip = resources->source<KisBrush>(ResourceType::Brushes)
                                   .bestMatch(QString(), QStringLiteral("Mountain_Brush_01.png"), QString())
                                   .dynamicCast<KisBrush>();
        QVERIFY(tip);
        std::optional<KisBrushModel::BrushData> brush = KisBrushModel::BrushData::read(settings.data(), resources);
        QVERIFY(brush);
        KisPredefinedBrushFactory::loadFromBrushResource(brush->common, brush->predefinedBrush, tip);
        brush->type = KisBrushModel::Predefined;
        brush->predefinedBrush.application = LIGHTNESSMAP;
        brush->write(settings.data());
        settings->setProperty("MergedPaint", true);
        settings->setProperty("PressurePaintThickness", true);
    }
}

/// What KisPaintopBox::slotGuiChangedCurrentPreset() does after the Brush
/// Editor has read a preset: clear it and let every option write again.
QMap<QString, QVariant> fullRewrite(KisPaintOpPresetSP preset)
{
    QScopedPointer<KisColorSmudgeOpSettingsWidget> widget(
        new KisColorSmudgeOpSettingsWidget(nullptr,
                                           KisGlobalResourcesInterface::instance(),
                                           KoCanvasResourcesInterfaceSP()));
    widget->setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget->setConfigurationSafe(preset->settings());

    KisPaintOpSettingsSP settings = preset->settings();
    settings->resetSettings();
    widget->writeConfigurationSafe(settings);
    return settings->getProperties();
}

/// One line per property; backslashes and line breaks in values are escaped.
QStringList serialize(const QMap<QString, QVariant> &properties)
{
    QStringList lines;
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        QString value = it.value().toString();
        value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
        value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        lines << it.key() + QLatin1Char('=') + value;
    }
    return lines;
}
} // namespace

void KisColorSmudgeParityTest::testLegacyRewrite_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("variant");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 10);
    for (const QString &file : files) {
        for (const QString &variant : {QStringLiteral("base"), QStringLiteral("new"), QStringLiteral("lightness")}) {
            QTest::newRow(qPrintable(file + "/" + variant)) << file << variant;
        }
    }
}

void KisColorSmudgeParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    QFETCH(QString, variant);
    KisPaintOpPresetSP preset = loadPreset(fileName);
    QVERIFY(preset);
    applyVariant(preset->settings(), variant);

    const QStringList actual = serialize(fullRewrite(preset));
    const QString referencePath =
        QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + "." + variant + ".properties");

    if (qEnvironmentVariableIsSet("SOLSTICE_WRITE_REFERENCES")) {
        QFile file(referencePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write((actual.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        return;
    }

    QFile file(referencePath);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(referencePath));
    const QStringList expected = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    int differences = 0;
    for (int i = 0; i < qMax(expected.size(), actual.size()); ++i) {
        const QString e = i < expected.size() ? expected[i] : QString();
        const QString a = i < actual.size() ? actual[i] : QString();
        if (e != a && differences++ < 5) {
            qInfo().noquote() << "expected:" << e.left(300);
            qInfo().noquote() << "actual:  " << a.left(300);
        }
    }
    QCOMPARE(differences, 0);
}

namespace
{
/// Every key of the preset, except those the options do not own.
QStringList presetOptionProperties(KisPaintOpSettingsSP settings)
{
    QMap<QString, QVariant> properties = settings->getProperties();
    properties.remove(QStringLiteral("paintop"));
    properties.remove(QStringLiteral("lodUserAllowed"));
    properties.remove(QStringLiteral("lodSizeThreshold"));
    return serialize(properties);
}

QStringList fullModelWrite(KisPaintOpOptionsModel *model)
{
    // writeAll() writes only to settings with an update listener
    KisPropertiesConfigurationSP config(new KisPropertiesConfiguration());
    Q_FOREACH (KisPaintOpOptionStateBase *option, model->options()) {
        option->write(config.data());
    }
    return serialize(config->getProperties());
}

template<typename Data>
KisPaintOpOptionState<Data> *typedOption(KisPaintOpOptionsModel *model, const QString &id)
{
    return dynamic_cast<KisPaintOpOptionState<Data> *>(model->option(id));
}
} // namespace

/// Color Smudge on the shared model: after every edit the preset holds what a
/// full write of the model writes, including the options whose written data
/// depends on the edited one (Smudge Length and, through it, Smudge Radius,
/// Paint Thickness and Overlay Mode on the brush tip).
void KisColorSmudgeParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("i_Wet_Circle.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();
    settings->setProperty("SmudgeRadiusVersion", 2);
    settings->setProperty("SmudgeRadiusValue", 2.5);
    settings->setProperty("MergedPaint", true);
    settings->setProperty("PressurePaintThickness", true);

    KisColorSmudgeOpSettingsWidget widget(nullptr,
                                          KisGlobalResourcesInterface::instance(),
                                          KoCanvasResourcesInterfaceSP());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    // the first edit rewrites everything
    auto *opacity = typedOption<KisOpacityOptionData>(model, QStringLiteral("Opacity"));
    QVERIFY(opacity);
    KisOpacityOptionData opacityData = opacity->data();
    opacityData.strengthValue = 0.5;
    opacity->cursor().set(opacityData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    QCOMPARE(settings->getDouble("SmudgeRadiusValue"), 2.5);

    // the new engine narrows the smudge radius range
    auto *smudgeLength = typedOption<KisSmudgeLengthOptionData>(model, QStringLiteral("SmudgeLength"));
    QVERIFY(smudgeLength);
    KisSmudgeLengthOptionData lengthData = smudgeLength->data();
    lengthData.useNewEngine = true;
    smudgeLength->cursor().set(lengthData);
    QCOMPARE(settings->getDouble("SmudgeRadiusValue"), 1.0);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    lengthData.useNewEngine = false;
    smudgeLength->cursor().set(lengthData);
    QCOMPARE(settings->getDouble("SmudgeRadiusValue"), 2.5);
    QVERIFY(settings->getBool("MergedPaint"));
    QVERIFY(!settings->getBool("PressurePaintThickness"));

    // a color tip in lightness map mode forces the new engine, enables
    // paint thickness and disables the overlay mode
    auto *brushTip = dynamic_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    QVERIFY(brushTip);
    const KisBrushSP tip = KisGlobalResourcesInterface::instance()
                               ->source<KisBrush>(ResourceType::Brushes)
                               .bestMatch(QString(), QStringLiteral("Mountain_Brush_01.png"), QString())
                               .dynamicCast<KisBrush>();
    QVERIFY(tip);
    KisBrushTipOptionData tipData = brushTip->data();
    KisPredefinedBrushFactory::loadFromBrushResource(tipData.brush.common, tipData.brush.predefinedBrush, tip);
    tipData.brush.type = KisBrushModel::Predefined;
    tipData.brush.predefinedBrush.application = LIGHTNESSMAP;
    brushTip->cursor().set(tipData);
    QVERIFY(settings->getBool("SmudgeRateUseNewEngine"));
    QCOMPARE(settings->getDouble("SmudgeRadiusValue"), 1.0);
    QVERIFY(settings->getBool("PressurePaintThickness"));
    QVERIFY(!settings->getBool("MergedPaint"));
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

/// Color Smudge options can be shown in Tool Options: unique ids, eyes on
/// the checkable options and on the page parameters.
void KisColorSmudgeParityTest::testToolOptionsIds()
{
    KisColorSmudgeOpSettingsWidget widget(nullptr,
                                          KisGlobalResourcesInterface::instance(),
                                          KoCanvasResourcesInterfaceSP());
    widget.setPaintOpId(QStringLiteral("colorsmudge"));

    QStringList ids;
    Q_FOREACH (KisPaintOpOption *option, widget.toolOptionsOptions()) {
        ids << option->toolOptionsId();
    }
    QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size());
    for (const QString &id : {QStringLiteral("BrushTip"),
                              QStringLiteral("Opacity"),
                              QStringLiteral("Size"),
                              QStringLiteral("SmudgeLength"),
                              QStringLiteral("SmudgeRadius"),
                              QStringLiteral("ColorRate"),
                              QStringLiteral("PaintThickness"),
                              QStringLiteral("OverlayMode"),
                              QStringLiteral("Texture")}) {
        QVERIFY2(ids.contains(id), qPrintable(id));
    }
    Q_FOREACH (QToolButton *eye, widget.findChildren<QToolButton *>(QStringLiteral("ToolOptionsEye"))) {
        QVERIFY(!eye->isHidden());
    }
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisColorSmudgeParityTest,
                                      QStringLiteral("Krita_4_Default_Resources.bundle"),
                                      QStringLiteral("Krita_3_Default_Resources.bundle"),
                                      QStringLiteral("RGBA_brushes.bundle"))

#include "KisColorSmudgeParityTest.moc"
