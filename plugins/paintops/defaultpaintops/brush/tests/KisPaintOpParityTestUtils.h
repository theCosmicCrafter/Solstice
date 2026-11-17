/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPAINTOPPARITYTESTUTILS_H
#define KISPAINTOPPARITYTESTUTILS_H

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTest>
#include <QToolButton>

#include <KisBrushModel.h>
#include <KisGlobalResourcesInterface.h>
#include <KisPaintOpOptionsModel.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_brush.h>
#include <kis_paintop_option.h>
#include <kis_paintop_settings_widget.h>
#include <kis_predefined_brush_factory.h>

/**
 * Helpers for the parity tests of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): an engine's editor must
 * write the same keys and values for every preset as before its migration
 * to the options model. References are .properties files next to the
 * presets, written by the code before the migration with
 * SOLSTICE_WRITE_REFERENCES=1.
 */
namespace KisPaintOpParityTestUtils
{

inline KisPaintOpPresetSP loadPreset(const QString &dir, const QString &fileName)
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset(QDir(dir).filePath(fileName)));
    return preset->load(KisGlobalResourcesInterface::instance()) && preset->settings() ? preset : KisPaintOpPresetSP();
}

/// One line per property; backslashes and line breaks in values are escaped.
inline QStringList serialize(const QMap<QString, QVariant> &properties)
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

/// What KisPaintopBox::slotGuiChangedCurrentPreset() does after the Brush
/// Editor has read a preset: clear it and let every option write again.
inline QStringList fullRewrite(KisPaintOpSettingsWidget *widget, KisPaintOpPresetSP preset)
{
    widget->setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget->setConfigurationSafe(preset->settings());

    KisPaintOpSettingsSP settings = preset->settings();
    settings->resetSettings();
    widget->writeConfigurationSafe(settings);
    return serialize(settings->getProperties());
}

/// Replaces the brush tip with a color tip of the RGBA brushes bundle in
/// lightness map mode, which engines without that mode must not write.
inline void useColorTip(KisPaintOpSettingsSP settings)
{
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
}

/// Compares @p actual with the reference at @p referencePath, or writes the
/// reference when SOLSTICE_WRITE_REFERENCES is set.
inline void compareWithReference(const QStringList &actual, const QString &referencePath)
{
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

/// Every key of the preset, except those the options do not own.
inline QStringList presetOptionProperties(KisPaintOpSettingsSP settings)
{
    QMap<QString, QVariant> properties = settings->getProperties();
    properties.remove(QStringLiteral("paintop"));
    properties.remove(QStringLiteral("lodUserAllowed"));
    properties.remove(QStringLiteral("lodSizeThreshold"));
    return serialize(properties);
}

inline QStringList fullModelWrite(KisPaintOpOptionsModel *model)
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

/// The options of @p widget can be shown in Tool Options: unique ids, the
/// @p expected ones among them, and the eyes shown once the engine is set.
inline void
verifyToolOptionsIds(KisPaintOpSettingsWidget *widget, const QString &paintOpId, const QStringList &expected)
{
    widget->setPaintOpId(paintOpId);

    QStringList ids;
    Q_FOREACH (KisPaintOpOption *option, widget->toolOptionsOptions()) {
        ids << option->toolOptionsId();
    }
    QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size());
    for (const QString &id : expected) {
        QVERIFY2(ids.contains(id), qPrintable(id));
    }
    Q_FOREACH (QToolButton *eye, widget->findChildren<QToolButton *>(QStringLiteral("ToolOptionsEye"))) {
        QVERIFY(!eye->isHidden());
    }
}

} // namespace KisPaintOpParityTestUtils

#endif // KISPAINTOPPARITYTESTUTILS_H
