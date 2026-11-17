/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

#include "KisBrushTestMain.h"
#include <testutil.h>

#include <KisGlobalResourcesInterface.h>
#include <KisPaintOpPresetUpdateProxy.h>
#include <KisResourceLoaderRegistry.h>
#include <KisResourceLocator.h>
#include <KisResourceModel.h>
#include <KisResourceModelProvider.h>
#include <KisResourceStorage.h>
#include <KisResourceTypes.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <brushengine/kis_uniform_paintop_property.h>

#include <KisBrushBasedOptionStates.h>
#include <KisMaskingBrushOption.h>
#include <KisPaintOpOptionsModel.h>
#include <KisPaintingModeOptionData.h>
#include <KisStandardOptionData.h>
#include <kis_brush_option_widget.h>
#include <lager/state.hpp>

#include "../kis_brushop_settings_widget.h"

/**
 * Phase 2a of the brush option shared model
 * (docs/agent/brush-option-shared-model.md): the brush tip and masking
 * brush options moved their state into one value each. The Pixel Brush editor
 * must still write the same keys and values for every preset, and the options
 * must behave the same on a shared model's cursor as on their own state.
 *
 * The references in the .properties files in data/brushtip were written by
 * the code before phase 2a (SOLSTICE_WRITE_REFERENCES=1 writes them again).
 */
class KisBrushTipOptionParityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testLegacyRewrite_data();
    void testLegacyRewrite();
    void testBrushTipExternalCursor_data();
    void testBrushTipExternalCursor();
    void testBrushTipCursorChangeReachesWidget();
    void testMaskingExternalCursor_data();
    void testMaskingExternalCursor();
    void testMaskingPreserveMode();
    void testLightnessMode();
    void testResizeWhileEditorOpen();
    void testEditAfterResizeKeepsSize();
    void testModelEditsKeepPresetConsistent();
    void testModelLightnessStrengthFollowsBrushTip();
};

namespace
{
QString dataDir()
{
    return QDir(QString(FILES_DATA_DIR)).filePath(QStringLiteral("brushtip"));
}

/// What KisPaintopBox::slotGuiChangedCurrentPreset() does after the Brush
/// Editor has read a preset: clear it and let every option write again.
QMap<QString, QVariant> fullRewrite(KisPaintOpPresetSP preset)
{
    QScopedPointer<KisBrushOpSettingsWidget> widget(
        new KisBrushOpSettingsWidget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP()));
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

/// The flags of the Pixel Brush.
const KisBrushOptionWidgetFlags pixelBrushFlags =
    KisBrushOptionWidgetFlag::SupportsHSLBrushMode | KisBrushOptionWidgetFlag::SupportsPrecision;

KisPaintOpPresetSP loadPreset(const QString &fileName)
{
    KisPaintOpPresetSP preset(new KisPaintOpPreset(QDir(dataDir()).filePath(fileName)));
    return preset->load(KisGlobalResourcesInterface::instance()) && preset->settings() ? preset : KisPaintOpPresetSP();
}

void addPresetRows(bool maskedOnly)
{
    QTest::addColumn<QString>("fileName");
    const QStringList files = QDir(dataDir()).entryList({QStringLiteral("*.kpp")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 16);
    for (const QString &file : files) {
        if (maskedOnly) {
            KisPaintOpPresetSP preset = loadPreset(file);
            QVERIFY(preset);
            if (!preset->settings()->getBool(QStringLiteral("MaskingBrush/Enabled"))) {
                continue;
            }
        }
        QTest::newRow(qPrintable(file)) << file;
    }
}

template<class Option>
QStringList writtenProperties(const Option &option)
{
    KisPropertiesConfigurationSP config(new KisPropertiesConfiguration());
    option.writeOptionSetting(config);
    return serialize(config->getProperties());
}

// A brush tip option does not own its configuration page (the editor's
// page stack does). Tests that create the option alone delete the page, or
// its preview timers fire after the option is gone.

/// The brush tip size of a preset, which the masking brush uses as its master size.
qreal brushTipSize(KisPaintOpPresetSP preset)
{
    KisBrushTipOptionData tip;
    tip.read(preset->settings().data(), KisGlobalResourcesInterface::instance(), pixelBrushFlags);
    return tip.commonBrushSize;
}
} // namespace

void KisBrushTipOptionParityTest::initTestCase()
{
    for (const QString &type : {ResourceType::Brushes, ResourceType::Patterns}) {
        QVERIFY2(KisResourceModelProvider::resourceModel(type)->rowCount() > 0, qPrintable(type));
    }
}

void KisBrushTipOptionParityTest::testLegacyRewrite_data()
{
    addPresetRows(false);
}

void KisBrushTipOptionParityTest::testLegacyRewrite()
{
    QFETCH(QString, fileName);
    KisPaintOpPresetSP preset = loadPreset(fileName);
    QVERIFY(preset);

    const QStringList actual = serialize(fullRewrite(preset));
    const QString referencePath = QDir(dataDir()).filePath(QFileInfo(fileName).completeBaseName() + ".properties");

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

void KisBrushTipOptionParityTest::testBrushTipExternalCursor_data()
{
    addPresetRows(false);
}

/// A brush tip option on a shared model's cursor writes what one with its own
/// state writes.
void KisBrushTipOptionParityTest::testBrushTipExternalCursor()
{
    QFETCH(QString, fileName);
    KisPaintOpPresetSP preset = loadPreset(fileName);
    QVERIFY(preset);

    KisBrushOptionWidget legacy(pixelBrushFlags);
    const QScopedPointer<QWidget> legacyPage(legacy.configurationPage());
    legacy.setResourcesInterface(KisGlobalResourcesInterface::instance());
    legacy.readOptionSetting(preset->settings());

    lager::state<KisBrushTipOptionData, lager::automatic_tag> state;
    KisBrushOptionWidget shared(pixelBrushFlags, state);
    const QScopedPointer<QWidget> sharedPage(shared.configurationPage());
    shared.setResourcesInterface(KisGlobalResourcesInterface::instance());
    shared.readOptionSetting(preset->settings());

    QCOMPARE(writtenProperties(shared), writtenProperties(legacy));
    QCOMPARE(state->commonBrushSize, legacy.effectiveBrushSize().get());
    QCOMPARE(shared.lightnessModeEnabled().get(), legacy.lightnessModeEnabled().get());
}

/// A change made through the cursor reaches the widget and what it writes.
void KisBrushTipOptionParityTest::testBrushTipCursorChangeReachesWidget()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("b_Basic-5_Size_Opacity.kpp"));
    QVERIFY(preset);

    lager::state<KisBrushTipOptionData, lager::automatic_tag> state;
    KisBrushOptionWidget widget(pixelBrushFlags, state);
    const QScopedPointer<QWidget> widgetPage(widget.configurationPage());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    widget.readOptionSetting(preset->settings());
    QCOMPARE(state->brush.type, KisBrushModel::Auto);

    int changes = 0;
    connect(&widget, &KisPaintOpOption::sigSettingChanged, this, [&changes]() {
        changes++;
    });

    KisBrushTipOptionData data = state.get();
    data.commonBrushSize = 42.0;
    state.set(data);

    QVERIFY(changes > 0);
    QCOMPARE(widget.effectiveBrushSize().get(), 42.0);

    KisPropertiesConfigurationSP config(new KisPropertiesConfiguration());
    widget.writeOptionSetting(config);
    KisBrushTipOptionData written;
    QVERIFY(written.read(config.data(), KisGlobalResourcesInterface::instance(), pixelBrushFlags));
    QCOMPARE(written.commonBrushSize, 42.0);
}

void KisBrushTipOptionParityTest::testMaskingExternalCursor_data()
{
    addPresetRows(true);
}

/// A masking brush option on a shared model's cursor writes what one with its
/// own state writes, also after the master size has changed.
void KisBrushTipOptionParityTest::testMaskingExternalCursor()
{
    QFETCH(QString, fileName);
    KisPaintOpPresetSP preset = loadPreset(fileName);
    QVERIFY(preset);

    lager::state<qreal, lager::automatic_tag> masterSize(brushTipSize(preset));

    KisMaskingBrushOption legacy(masterSize);
    legacy.setResourcesInterface(KisGlobalResourcesInterface::instance());
    legacy.readOptionSetting(preset->settings());

    lager::state<KisMaskingBrushOptionData, lager::automatic_tag> state;
    KisMaskingBrushOption shared(state, masterSize);
    shared.setResourcesInterface(KisGlobalResourcesInterface::instance());
    shared.readOptionSetting(preset->settings());

    QVERIFY(state->preserveMode);
    QCOMPARE(writtenProperties(shared), writtenProperties(legacy));

    // The master size belongs to the brush tip option, which notifies the
    // change itself. Ending the preserve mode must not notify: the editor may
    // be reading the brush tip, and a notification starts a full rewrite.
    int changes = 0;
    connect(&legacy, &KisPaintOpOption::sigSettingChanged, this, [&changes]() {
        changes++;
    });
    connect(&shared, &KisPaintOpOption::sigSettingChanged, this, [&changes]() {
        changes++;
    });
    const QStringList preserved = writtenProperties(shared);

    masterSize.set(masterSize.get() * 2.0);

    QVERIFY(!state->preserveMode);
    QCOMPARE(changes, 0);
    QCOMPARE(writtenProperties(shared), writtenProperties(legacy));
    QVERIFY(writtenProperties(shared) != preserved);
}

/// The stored size coefficient is written back unchanged until the master
/// size or the masking brush size changes.
void KisBrushTipOptionParityTest::testMaskingPreserveMode()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("h_Charcoal_Pencil_Medium.kpp"));
    QVERIFY(preset);
    const qreal master = brushTipSize(preset);
    const qreal stored = preset->settings()->getDouble(QStringLiteral("MaskingBrush/MasterSizeCoeff"));

    KisMaskingBrushOptionData data;
    data.read(preset->settings().data(), master, KisGlobalResourcesInterface::instance());
    QVERIFY(data.masking.useMasterSize);
    QVERIFY(data.preserveModeHolds(master));
    QCOMPARE(data.bakedMaskingData(master).masterSizeCoeff, stored);

    const qreal otherMaster = master * 2.0;
    QVERIFY(!data.preserveModeHolds(otherMaster));
    QCOMPARE(data.bakedMaskingData(otherMaster).masterSizeCoeff, data.commonBrushSize / otherMaster);

    KisMaskingBrushOptionData resized = data;
    resized.commonBrushSize *= 0.5;
    QVERIFY(!resized.preserveModeHolds(master));
    QCOMPARE(resized.bakedMaskingData(master).masterSizeCoeff, resized.commonBrushSize / master);

    resized.startPreserveMode(master);
    QVERIFY(resized.preserveModeHolds(master));
    QCOMPARE(resized.bakedMaskingData(master).masterSizeCoeff, stored);
}

/// The lightness map needs an image tip and an engine with HSL brush modes;
/// otherwise the effective application falls back as the editor shows it.
void KisBrushTipOptionParityTest::testLightnessMode()
{
    KisBrushTipOptionData data;
    data.brush.type = KisBrushModel::Predefined;
    data.brush.predefinedBrush.resourceSignature = KoResourceSignature(ResourceType::Brushes,
                                                                       QStringLiteral("md5"),
                                                                       QStringLiteral("tip.png"),
                                                                       QStringLiteral("tip"));
    data.brush.predefinedBrush.brushType = IMAGE;
    data.brush.predefinedBrush.baseSize = QSize(100, 80);
    data.brush.predefinedBrush.application = LIGHTNESSMAP;
    data.commonBrushSize = 50.0;

    QVERIFY(data.lightnessModeEnabled(pixelBrushFlags));
    QCOMPARE(data.bakedBrushData(pixelBrushFlags).predefinedBrush.application, LIGHTNESSMAP);
    QCOMPARE(data.bakedBrushData(pixelBrushFlags).predefinedBrush.scale, 0.5);

    const KisBrushOptionWidgetFlags noHsl = KisBrushOptionWidgetFlag::SupportsPrecision;
    QVERIFY(!data.lightnessModeEnabled(noHsl));
    QCOMPARE(data.bakedBrushData(noHsl).predefinedBrush.application, IMAGESTAMP);

    data.brush.predefinedBrush.brushType = MASK;
    QVERIFY(!data.lightnessModeEnabled(pixelBrushFlags));
    QCOMPARE(data.bakedBrushData(pixelBrushFlags).predefinedBrush.application, ALPHAMASK);

    data.brush.type = KisBrushModel::Auto;
    data.brush.predefinedBrush.brushType = IMAGE;
    QVERIFY(!data.lightnessModeEnabled(pixelBrushFlags));
}

namespace
{
int s_safeAsserts = 0;
QtMessageHandler s_previousHandler = nullptr;

void countSafeAsserts(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains(QStringLiteral("SAFE ASSERT"))) {
        s_safeAsserts++;
    }
    s_previousHandler(type, context, message);
}
} // namespace

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
    // writeAll() goes through the locked-properties proxy, which writes only
    // to settings with an update listener; no option is locked here
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

QSet<QString> changedKeys(const QMap<QString, QVariant> &before, const QMap<QString, QVariant> &after)
{
    QSet<QString> keys;
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        if (!after.contains(it.key()) || after.value(it.key()) != it.value()) {
            keys << it.key();
        }
    }
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (!before.contains(it.key())) {
            keys << it.key();
        }
    }
    return keys;
}
} // namespace

/// Pixel Brush on the shared model: after every edit the preset holds what a
/// full write of the model writes, including the options whose written data
/// depends on the edited one (painting mode on the masking brush, the masking
/// size coefficient and Lightness Strength on the brush tip).
void KisBrushTipOptionParityTest::testModelEditsKeepPresetConsistent()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("h_Charcoal_Pencil_Medium.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisBrushOpSettingsWidget widget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP());
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

    // later edits write only their option
    auto *flow = typedOption<KisFlowOptionData>(model, QStringLiteral("Flow"));
    QVERIFY(flow);
    const QMap<QString, QVariant> beforeFlow = settings->getProperties();
    KisFlowOptionData flowData = flow->data();
    flowData.strengthValue = 0.25;
    flow->cursor().set(flowData);
    const QSet<QString> flowKeys = changedKeys(beforeFlow, settings->getProperties());
    QVERIFY(!flowKeys.isEmpty());
    QVERIFY(model->optionKeys(QStringLiteral("Flow")).contains(flowKeys));
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));

    // the painting mode is wash while the masking brush is enabled
    auto *paintingMode = typedOption<KisPaintingModeOptionData>(model, QStringLiteral("PaintingMode"));
    QVERIFY(paintingMode);
    KisPaintingModeOptionData paintingModeData = paintingMode->data();
    paintingModeData.paintingMode = enumPaintingMode::BUILDUP;
    paintingMode->cursor().set(paintingModeData);
    const QVariant washAction = settings->getProperty(QStringLiteral("PaintOpAction"));

    auto *masking = dynamic_cast<KisMaskingBrushOptionState *>(model->option(QStringLiteral("MaskingBrush")));
    QVERIFY(masking);
    QVERIFY(masking->data().masking.isEnabled);
    KisMaskingBrushOptionData maskingData = masking->data();
    maskingData.masking.isEnabled = false;
    masking->cursor().set(maskingData);
    QVERIFY(settings->getProperty(QStringLiteral("PaintOpAction")) != washAction);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
    maskingData = masking->data();
    maskingData.masking.isEnabled = true;
    masking->cursor().set(maskingData);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));

    // a new brush tip size ends the preserve mode and changes the coefficient
    auto *brushTip = dynamic_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    QVERIFY(brushTip);
    const QVariant coefficient = settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff"));
    KisBrushTipOptionData tipData = brushTip->data();
    tipData.commonBrushSize *= 1.5;
    brushTip->cursor().set(tipData);
    QVERIFY(settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff")) != coefficient);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));

    // outside the preserve mode only the brush tip changes; the masking
    // brush writes the new coefficient as a dependent option
    QVERIFY(!masking->data().preserveMode);
    const QVariant secondCoefficient = settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff"));
    tipData = brushTip->data();
    tipData.commonBrushSize *= 1.5;
    brushTip->cursor().set(tipData);
    QVERIFY(settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff")) != secondCoefficient);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

/// Lightness Strength is written enabled only while the brush tip is in the
/// lightness mode; switching the tip rewrites it.
void KisBrushTipOptionParityTest::testModelLightnessStrengthFollowsBrushTip()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("b_Basic-6_Details.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisBrushOpSettingsWidget widget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);

    auto *brushTip = dynamic_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    auto *lightness = typedOption<KisLightnessStrengthOptionData>(model, QStringLiteral("LightnessStrength"));
    QVERIFY(brushTip && lightness);
    QCOMPARE(brushTip->data().brush.type, KisBrushModel::Predefined);

    KisLightnessStrengthOptionData lightnessData = lightness->data();
    lightnessData.isChecked = true;
    lightness->cursor().set(lightnessData);
    QVERIFY(!brushTip->data().lightnessModeEnabled(brushTip->flags()));
    QCOMPARE(settings->getBool(QStringLiteral("PressureLightnessStrength")), false);

    KisBrushTipOptionData tipData = brushTip->data();
    tipData.brush.predefinedBrush.brushType = IMAGE;
    tipData.brush.predefinedBrush.application = LIGHTNESSMAP;
    brushTip->cursor().set(tipData);
    QVERIFY(brushTip->data().lightnessModeEnabled(brushTip->flags()));
    QCOMPARE(settings->getBool(QStringLiteral("PressureLightnessStrength")), true);
    QCOMPARE(presetOptionProperties(settings), fullModelWrite(model));
}

/// Resizing the brush on the canvas (Shift + drag) while the Brush Editor is
/// open, wired as KisPaintopBox does: the tool writes the size through the
/// settings, the options model reads the changed keys, and an editor change
/// writes back (a full rewrite only without an attached model).
void KisBrushTipOptionParityTest::testResizeWhileEditorOpen()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("h_Charcoal_Pencil_Medium.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisBrushOpSettingsWidget widget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);
    widget.setConfigurationSafe(settings);

    bool insideRead = false;
    bool insideWrite = false;
    connect(&widget, &KisPaintOpConfigWidget::sigConfigurationUpdated, this, [&]() {
        if (insideRead)
            return;
        insideWrite = true;
        {
            KisPaintOpPreset::UpdatedPostponer postponer(preset);
            if (!model->isAttachedTo(settings.data())) {
                settings->resetSettings();
            }
            widget.writeConfigurationSafe(settings);
        }
        insideWrite = false;
    });
    connect(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChangedUncompressed, this, [&]() {
        if (insideWrite)
            return;
        insideRead = true;
        widget.setConfigurationSafe(settings);
        insideRead = false;
    });

    QList<KisUniformPaintOpPropertySP> properties = settings->uniformProperties(settings, preset->updateProxy());
    QVERIFY(!properties.isEmpty());

    s_safeAsserts = 0;
    s_previousHandler = qInstallMessageHandler(countSafeAsserts);
    const QVariant coefficient = settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff"));
    qreal size = settings->paintOpSize();
    for (int i = 0; i < 20; i++) {
        size += 3.0;
        settings->setPaintOpSize(size);
        QTest::qWait(30);
    }
    QTest::qWait(500);
    qInstallMessageHandler(s_previousHandler);

    QCOMPARE(s_safeAsserts, 0);
    QVERIFY(settings->hasProperty(QStringLiteral("brush_definition")));
    QCOMPARE(settings->paintOpSize(), size);

    // the model followed the size, and the masking brush still writes the
    // coefficient of the preset (nothing was edited in the editor)
    KisBrushTipOptionState *brushTip = static_cast<KisBrushTipOptionState *>(model->option(QStringLiteral("BrushTip")));
    QCOMPARE(brushTip->data().commonBrushSize, size);
    QCOMPARE(settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff")), coefficient);
}

/// After resizing on the canvas, toggling another option in the Brush Editor
/// (Rotation) keeps the new size and the masking brush's master size
/// coefficient (reported 2026-10-11: the first edit rewrote the coefficient
/// for the old size, so the masked stroke became thin and stayed thin).
void KisBrushTipOptionParityTest::testEditAfterResizeKeepsSize()
{
    KisPaintOpPresetSP preset = loadPreset(QStringLiteral("h_Charcoal_Pencil_Medium.kpp"));
    QVERIFY(preset);
    KisPaintOpSettingsSP settings = preset->settings();

    KisBrushOpSettingsWidget widget(nullptr, KisGlobalResourcesInterface::instance(), KoCanvasResourcesInterfaceSP());
    widget.setResourcesInterface(KisGlobalResourcesInterface::instance());
    KisPaintOpOptionsModel *model = widget.optionsModel();
    QVERIFY(model);
    model->attachPreset(preset);
    widget.setConfigurationSafe(settings);

    bool insideRead = false;
    bool insideWrite = false;
    connect(&widget, &KisPaintOpConfigWidget::sigConfigurationUpdated, this, [&]() {
        if (insideRead)
            return;
        insideWrite = true;
        {
            KisPaintOpPreset::UpdatedPostponer postponer(preset);
            if (!model->isAttachedTo(settings.data())) {
                settings->resetSettings();
            }
            widget.writeConfigurationSafe(settings);
        }
        insideWrite = false;
    });
    connect(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChangedUncompressed, this, [&]() {
        if (insideWrite)
            return;
        insideRead = true;
        widget.setConfigurationSafe(settings);
        insideRead = false;
    });

    const qreal original = settings->paintOpSize();
    const QVariant coefficient = settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff"));
    const qreal size = original * 3;
    settings->setPaintOpSize(size);
    QTest::qWait(300);
    QCOMPARE(settings->paintOpSize(), size);

    auto *rotation = typedOption<KisRotationOptionData>(model, QStringLiteral("Rotation"));
    QVERIFY(rotation);
    for (bool checked : {true, false}) {
        KisRotationOptionData data = rotation->data();
        data.isChecked = checked;
        rotation->cursor().set(data);
        QTest::qWait(300);
        QVERIFY(qAbs(settings->paintOpSize() - size) < 0.01);
        QCOMPARE(settings->getProperty(QStringLiteral("MaskingBrush/MasterSizeCoeff")), coefficient);
    }
}

SOLSTICE_BRUSH_TEST_MAIN(KisBrushTipOptionParityTest)

#include "KisBrushTipOptionParityTest.moc"
