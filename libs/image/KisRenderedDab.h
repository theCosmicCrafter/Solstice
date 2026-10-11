/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISRENDEREDDAB_H
#define KISRENDEREDDAB_H

#include "kis_types.h"
#include "kis_fixed_paint_device.h"

#include <QSharedPointer>

#include "KisProceduralCircleDab.h"

struct KisRenderedDab
{
    KisRenderedDab() {}
    KisRenderedDab(KisFixedPaintDeviceSP _device)
        : device(_device),
          offset(_device->bounds().topLeft())
    {
    }

    KisRenderedDab(const KisRenderedDab &/*rhs*/) = default;

    KisFixedPaintDeviceSP device;
    QPoint offset;

    qreal opacity = OPACITY_OPAQUE_F;
    qreal flow = OPACITY_OPAQUE_F;
    qreal averageOpacity = OPACITY_TRANSPARENT_F;

    /// GPU engine (Solstice): optional description of the device pixels
    /// (KisProceduralCircleDab), and the mirroring applied to the pixels
    /// since generation (horizontal = 1, vertical = 2).
    QSharedPointer<const KisProceduralCircleDab> procedural;
    quint32 proceduralFlips = 0;
    /// The device has its bounds but not its pixels yet (the CPU generation
    /// was skipped for a described dab). CPU users must call materialize().
    bool pixelsPending = false;

    inline QRect realBounds() const {
        return QRect(offset, device->bounds().size());
    }

    /// Renders pending pixels from the description (bit-exact with the CPU
    /// generator), including the reflections recorded in proceduralFlips.
    inline void materialize()
    {
        if (!pixelsPending || !procedural || !device) {
            return;
        }
        device->lazyGrowBufferWithoutInitialization();
        if (procedural->texture && procedural->texture->cpu) {
            // The circle as textured, the CPU texture, then the reflections
            // made since texturing.
            const quint32 brushFlips = procedural->texture->brushFlips & 3;
            procedural->render(device->data(), device->bounds().width(), device->bounds().height(), brushFlips);
            procedural->texture->cpu->apply(device);
            const quint32 later = (proceduralFlips & 3) ^ brushFlips;
            if (later) {
                device->mirror(later & 1, later & 2);
            }
        } else {
            procedural->render(device->data(),
                               device->bounds().width(),
                               device->bounds().height(),
                               proceduralFlips & 3);
        }
        pixelsPending = false;
    }

    static inline bool hasPendingPixels(const QList<KisRenderedDab> &dabs)
    {
        for (const KisRenderedDab &dab : dabs) {
            if (dab.pixelsPending) {
                return true;
            }
        }
        return false;
    }

    static inline void materialize(QList<KisRenderedDab> &dabs)
    {
        for (KisRenderedDab &dab : dabs) {
            dab.materialize();
        }
    }
};

#endif // KISRENDEREDDAB_H
