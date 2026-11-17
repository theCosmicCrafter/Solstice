/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISMASKINGBRUSHOPTION_H
#define KISMASKINGBRUSHOPTION_H

#include <kritapaintop_export.h>
#include <QScopedPointer>

#include <kis_types.h>
#include "kis_paintop_option.h"
#include <lager/reader.hpp>
#include <lager/cursor.hpp>

struct KisMaskingBrushOptionData;

class PAINTOP_EXPORT KisMaskingBrushOption : public KisPaintOpOption
{
    Q_OBJECT
public:
    /// The option owns its state.
    KisMaskingBrushOption(lager::reader<qreal> effectiveBrushSize);
    /// Solstice: the state lives in @p optionData, e.g. a shared options
    /// model (docs/agent/brush-option-shared-model.md).
    KisMaskingBrushOption(lager::cursor<KisMaskingBrushOptionData> optionData,
                          lager::reader<qreal> effectiveBrushSize);
    ~KisMaskingBrushOption() override;

    void writeOptionSetting(KisPropertiesConfigurationSP setting) const override;
    void readOptionSetting(const KisPropertiesConfigurationSP setting) override;

    void setImage(KisImageWSP image) override;

    void lodLimitations(KisPaintopLodLimitations *l) const override;

    lager::reader<bool> maskingBrushEnabledReader() const;

private Q_SLOTS:
    void slotCompositeModeWidgetChanged(int index);
    void slotCompositeModePropertyChanged(const QString &value);

private:
    struct Private;
    // Takes the private data, so that a lager::state argument does not make
    // the public constructors ambiguous.
    KisMaskingBrushOption(Private *d);

    const QScopedPointer<Private> m_d;
};



#endif // KISMASKINGBRUSHOPTION_H
