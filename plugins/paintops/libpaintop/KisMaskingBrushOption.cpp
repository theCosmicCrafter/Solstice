/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisMaskingBrushOption.h"

#include "kis_predefined_brush_chooser.h"
#include "kis_brush_selection_widget.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>

#include <QDomDocument>
#include "kis_brush.h"
#include "kis_image.h"
#include "kis_image_config.h"
#include "kis_brush_option.h"

#include "KisMaskingBrushOptionProperties.h"
#include <strokes/KisMaskingBrushCompositeOpFactory.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/KisPaintopSettingsIds.h>
#include <brushengine/kis_paintop_lod_limitations.h>
#include <lager/state.hpp>
#include <lager/constant.hpp>
#include <KisWidgetConnectionUtils.h>
#include <functional>
#include "KisAutoBrushModel.h"
#include "KisPredefinedBrushModel.h"
#include "KisTextBrushModel.h"
#include "KisBrushTipOptionData.h"

#include <optional>


using namespace KisBrushModel;
using namespace KisWidgetConnectionUtils;


namespace detail {

QString warningLabelText(qreal realBrushSize, qreal theoreticalMaskingBrushSize)
{
    return
        i18nc("warning about too big size of the masked brush",
              "WARNING: Dependent size of the masked brush grew too big (%1 pixels). Its value has been cropped to %2 pixels.",
              theoreticalMaskingBrushSize,
              realBrushSize);
}

bool warningLabelVisible(qreal theoreticalBrushSize) {
    KisImageConfig cfg(true);
    return theoreticalBrushSize > cfg.maxMaskingBrushSize();
}


}

class MaskingBrushModel : public QObject
{
    Q_OBJECT
public:
    MaskingBrushModel(lager::cursor<KisMaskingBrushOptionData> optionData, lager::reader<qreal> masterBrushSize)
        : m_optionData(optionData),
          m_maskingData(optionData[&KisMaskingBrushOptionData::masking]),
          m_commonBrushSizeData(optionData[&KisMaskingBrushOptionData::commonBrushSize]),
          m_masterBrushSize(masterBrushSize),
          m_preserveMode(optionData[&KisMaskingBrushOptionData::preserveMode]),
          autoBrushModel(m_maskingData[&MaskingBrushData::brush][&BrushData::common],
                         m_maskingData[&MaskingBrushData::brush][&BrushData::autoBrush],
                         m_commonBrushSizeData),
          predefinedBrushModel(m_maskingData[&MaskingBrushData::brush][&BrushData::common],
                               m_maskingData[&MaskingBrushData::brush][&BrushData::predefinedBrush],
                               m_commonBrushSizeData,
                               false),
          textBrushModel(m_maskingData[&MaskingBrushData::brush][&BrushData::common],
                         m_maskingData[&MaskingBrushData::brush][&BrushData::textBrush]),
          LAGER_QT(isEnabled) {m_maskingData[&MaskingBrushData::isEnabled]},
          LAGER_QT(compositeOpId) {m_maskingData[&MaskingBrushData::compositeOpId]},
          LAGER_QT(theoreticalBrushSize) {
              lager::with(m_maskingData[&MaskingBrushData::masterSizeCoeff],
                          m_masterBrushSize)
                      .map(std::multiplies<qreal>{})},
          LAGER_QT(realBrushSize) {m_commonBrushSizeData},
          LAGER_QT(warningLabelVisible) {
              lager::with(m_preserveMode,
                          LAGER_QT(theoreticalBrushSize).map(&detail::warningLabelVisible))
                      .map(std::logical_and<bool>{})},
          LAGER_QT(warningLabelText) {
              lager::with(LAGER_QT(realBrushSize),
                          LAGER_QT(theoreticalBrushSize))
                      .map(&detail::warningLabelText)}
    {
        lager::watch(m_optionData, std::bind(&MaskingBrushModel::updatePreserveMode, this));
        lager::watch(m_masterBrushSize, std::bind(&MaskingBrushModel::updatePreserveMode, this));
    }

    lager::cursor<KisMaskingBrushOptionData> m_optionData;
    lager::cursor<MaskingBrushData> m_maskingData;
    lager::cursor<qreal> m_commonBrushSizeData;
    lager::reader<qreal> m_masterBrushSize;
    lager::cursor<bool> m_preserveMode;

    KisAutoBrushModel autoBrushModel;
    KisPredefinedBrushModel predefinedBrushModel;
    KisTextBrushModel textBrushModel;

    LAGER_QT_CURSOR(bool, isEnabled);
    LAGER_QT_CURSOR(QString, compositeOpId);
    LAGER_QT_READER(qreal, theoreticalBrushSize);
    LAGER_QT_READER(qreal, realBrushSize);
    LAGER_QT_READER(bool, warningLabelVisible);
    LAGER_QT_READER(QString, warningLabelText);

    /// The preserve mode ends for good once the masking brush, the master size
    /// or the common size differs from what was read.
    void updatePreserveMode()
    {
        if (!m_preserveMode.get()) return;

        if (!m_optionData->preserveModeHolds(m_masterBrushSize.get())) {
            m_preserveMode.set(false);
        }
    }
};

struct KisMaskingBrushOption::Private
{
    Private(std::optional<lager::cursor<KisMaskingBrushOptionData>> externalData,
            lager::reader<qreal> effectiveBrushSize)
        : ui(new QWidget())
        , data(externalData ? *externalData : lager::cursor<KisMaskingBrushOptionData>(ownData))
        , masking(data[&KisMaskingBrushOptionData::masking])
        , commonBrushSize(data[&KisMaskingBrushOptionData::commonBrushSize])
        , masterBrushSize(effectiveBrushSize)
        , maskingModel(data, effectiveBrushSize)
    {
        compositeSelector = new QComboBox(ui.data());

        const QStringList supportedComposites = KisMaskingBrushCompositeOpFactory::supportedCompositeOpIds();
        Q_FOREACH (const QString &id, supportedComposites) {
            const QString name = KoCompositeOpRegistry::instance().getKoID(id).name();
            compositeSelector->addItem(name, id);
        }
        compositeSelector->setCurrentIndex(0);

        QHBoxLayout *compositeOpLayout = new QHBoxLayout();
        compositeOpLayout->addWidget(new QLabel(i18n("Blending Mode:")), 0);
        compositeOpLayout->addWidget(compositeSelector, 1);

        brushSizeWarningLabel = new QLabel(ui.data());
        brushSizeWarningLabel->setVisible(false);
        brushSizeWarningLabel->setWordWrap(true);

        brushChooser = new KisBrushSelectionWidget(KisImageConfig(true).maxMaskingBrushSize(),
                                                   &maskingModel.autoBrushModel,
                                                   &maskingModel.predefinedBrushModel,
                                                   &maskingModel.textBrushModel,
                                                   data[&KisMaskingBrushOptionData::masking][&MaskingBrushData::brush][&BrushData::type],
                                                   brushPrecisionData,
                                                   KisBrushOptionWidgetFlag::None,
                                                   ui.data());

        QVBoxLayout *layout  = new QVBoxLayout(ui.data());
        layout->addLayout(compositeOpLayout, 0);
        layout->addWidget(brushSizeWarningLabel, 0);
        layout->addWidget(brushChooser, 1);
    }

    QScopedPointer<QWidget> ui;
    KisBrushSelectionWidget *brushChooser = 0;
    QComboBox *compositeSelector = 0;
    QLabel *brushSizeWarningLabel = 0;

    /// The state when the option owns it (no external cursor). Solstice: the
    /// masking data, its common size and the preserve mode are one value,
    /// KisMaskingBrushOptionData (docs/agent/brush-option-shared-model.md).
    lager::state<KisMaskingBrushOptionData, lager::automatic_tag> ownData;
    lager::cursor<KisMaskingBrushOptionData> data;
    /// The parts that notify a change, as before the shared model. The
    /// preserve mode ends when another option (the brush tip) changes the
    /// master size, possibly while the editor is reading that option; a
    /// notification then would start a full rewrite inside the read.
    lager::reader<MaskingBrushData> masking;
    lager::reader<qreal> commonBrushSize;
    lager::reader<qreal> masterBrushSize;
    MaskingBrushModel maskingModel;

    /// we don't use precision data, we just need it to pass
    /// to the brush selection widget
    lager::state<KisBrushModel::PrecisionData, lager::automatic_tag> brushPrecisionData;
};


KisMaskingBrushOption::KisMaskingBrushOption(lager::reader<qreal> effectiveBrushSize)
    : KisMaskingBrushOption(new Private(std::nullopt, effectiveBrushSize))
{
}

KisMaskingBrushOption::KisMaskingBrushOption(lager::cursor<KisMaskingBrushOptionData> optionData,
                                             lager::reader<qreal> effectiveBrushSize)
    : KisMaskingBrushOption(new Private(optionData, effectiveBrushSize))
{
}

KisMaskingBrushOption::KisMaskingBrushOption(Private *d)
    : KisPaintOpOption(i18n("Brush Tip"), KisPaintOpOption::MASKING_BRUSH, true)
    , m_d(d)
{
    setObjectName("KisMaskingBrushOption");
    setConfigurationPage(m_d->ui.data());

    connect(&m_d->maskingModel, &MaskingBrushModel::isEnabledChanged,
            this, &KisMaskingBrushOption::setChecked);
    connect(this, &KisMaskingBrushOption::sigCheckedChanged,
            &m_d->maskingModel, &MaskingBrushModel::setisEnabled);
    m_d->maskingModel.LAGER_QT(isEnabled).nudge();

    connect(&m_d->maskingModel, &MaskingBrushModel::compositeOpIdChanged,
            this, &KisMaskingBrushOption::slotCompositeModePropertyChanged);
    connect(m_d->compositeSelector, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &KisMaskingBrushOption::slotCompositeModeWidgetChanged);
    m_d->maskingModel.LAGER_QT(compositeOpId).nudge();

    connect(&m_d->maskingModel, &MaskingBrushModel::warningLabelVisibleChanged,
            m_d->brushSizeWarningLabel, &QLabel::setVisible);
    m_d->maskingModel.LAGER_QT(warningLabelVisible).nudge();

    connect(&m_d->maskingModel, &MaskingBrushModel::warningLabelTextChanged,
            m_d->brushSizeWarningLabel, &QLabel::setText);
    m_d->maskingModel.LAGER_QT(warningLabelText).nudge();

    m_d->masking.watch(std::bind(&KisMaskingBrushOption::emitSettingChanged, this));
    m_d->commonBrushSize.watch(std::bind(&KisMaskingBrushOption::emitSettingChanged, this));
}

KisMaskingBrushOption::~KisMaskingBrushOption()
{
}

void KisMaskingBrushOption::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->data->write(setting.data(), m_d->masterBrushSize.get());
}

void KisMaskingBrushOption::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisMaskingBrushOptionData data = m_d->data.get();
    data.read(setting.data(), m_d->masterBrushSize.get(), resourcesInterface());
    m_d->data.set(data);

    // The size controls round the common size they were given and write it
    // back, which ends the preserve mode. Start it from the settled values,
    // as the editor did before the shared model.
    data = m_d->data.get();
    data.startPreserveMode(m_d->masterBrushSize.get());
    m_d->data.set(data);
}

void KisMaskingBrushOption::setImage(KisImageWSP image)
{
    m_d->brushChooser->setImage(image);
}

void KisMaskingBrushOption::lodLimitations(KisPaintopLodLimitations *l) const
{
    *l |= KisBrushModel::brushLodLimitations(m_d->data->masking.brush);
}

lager::reader<bool> KisMaskingBrushOption::maskingBrushEnabledReader() const
{
    return m_d->data[&KisMaskingBrushOptionData::masking][&MaskingBrushData::isEnabled];
}

void KisMaskingBrushOption::slotCompositeModeWidgetChanged(int index)
{
    m_d->maskingModel.setcompositeOpId(m_d->compositeSelector->itemData(index).toString());
}

void KisMaskingBrushOption::slotCompositeModePropertyChanged(const QString &value)
{
    const int index = m_d->compositeSelector->findData(QVariant::fromValue(value));
    KIS_SAFE_ASSERT_RECOVER_RETURN(index >= 0);
    m_d->compositeSelector->setCurrentIndex(index);
}

#include "KisMaskingBrushOption.moc"
