/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisDustDetector.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <KoColorSpace.h>
#include <KoMixColorsOp.h>
#include <KoUpdater.h>

#include "kis_paint_device.h"
#include "kis_pixel_selection.h"
#include "kis_random_accessor_ng.h"
#include "kis_sequential_iterator.h"

#ifdef HAVE_KRITA_GPU_ENGINE
#include "gpu/KisGpuTileAccess.h"
#endif

namespace
{

// Colors closer than this (0..255) belong to the same surrounding color.
constexpr quint8 surroundingClusterDifference = 12;

// Components with a pixel selected at least this much are inside an enclosure.
constexpr quint8 enclosureThreshold = 128;

enum PixelState : quint8 {
    Background = 0,
    Candidate,
    Visited,
    Surrounding,
};

class Classifier
{
public:
    Classifier(const KoColorSpace *cs, const KisDustDetector::Options &options)
        : m_cs(cs)
        , m_mode(options.mode)
        , m_tolerance(qBound(0.0, std::isfinite(options.tolerance) ? options.tolerance : 0.0, 1.0))
        , m_white(Qt::white, cs)
        , m_opaque(cs->pixelSize(), '\0')
    {
    }

    bool isCandidate(const quint8 *pixel)
    {
        const qreal opacity = m_cs->opacityF(pixel);

        switch (m_mode) {
        case KisDustDetector::DustOnTransparency:
            return opacity > m_tolerance;
        case KisDustDetector::DustOnWhite: {
            if (!(opacity > 0)) {
                return false;
            }
            // Compare the color only; semi-transparent white is still white.
            std::memcpy(m_opaque.data(), pixel, m_cs->pixelSize());
            m_cs->setOpacity(reinterpret_cast<quint8 *>(m_opaque.data()), OPACITY_OPAQUE_U8, 1);
            const quint8 difference =
                m_cs->difference(reinterpret_cast<const quint8 *>(m_opaque.constData()), m_white.data());
            return difference > qRound(m_tolerance * 255);
        }
        case KisDustDetector::TransparentGaps:
            return opacity < 1.0 - m_tolerance - 1e-6 || !std::isfinite(opacity);
        }

        return false;
    }

private:
    const KoColorSpace *m_cs;
    KisDustDetector::Mode m_mode;
    qreal m_tolerance;
    KoColor m_white;
    QByteArray m_opaque;
};

KoColor mainSurroundingColor(const KoColorSpace *cs, const QByteArray &pixels, int count)
{
    const int pixelSize = cs->pixelSize();
    const auto pixel = [&](int i) {
        return reinterpret_cast<const quint8 *>(pixels.constData()) + i * pixelSize;
    };

    // Group similar colors and pick the largest group, so a hole on the
    // border between two flat colors takes one of them instead of a mix.
    QVector<int> representatives;
    QVector<QVector<int>> members;
    for (int i = 0; i < count; ++i) {
        int group = -1;
        for (int j = 0; j < representatives.size(); ++j) {
            if (cs->differenceA(pixel(i), pixel(representatives[j])) <= surroundingClusterDifference) {
                group = j;
                break;
            }
        }
        if (group < 0) {
            representatives.append(i);
            members.append(QVector<int>());
            group = representatives.size() - 1;
        }
        members[group].append(i);
    }

    if (members.isEmpty()) {
        return KoColor::createTransparent(cs);
    }

    int largest = 0;
    for (int j = 1; j < members.size(); ++j) {
        if (members[j].size() > members[largest].size()) {
            largest = j;
        }
    }

    QVector<const quint8 *> colors;
    for (int i : members[largest]) {
        colors.append(pixel(i));
    }

    KoColor result(cs);
    cs->mixColorsOp()->mixColors(colors.constData(), colors.size(), result.data());
    return result;
}

} // namespace

QRect KisDustDetector::neededRect(const QRect &rect, int dustSize)
{
    const int margin = qBound(minimumDustSize, dustSize, maximumDustSize) + 1;
    return rect.isEmpty() ? rect : rect.adjusted(-margin, -margin, margin, margin);
}

KisDustDetector::Replacement KisDustDetector::defaultReplacement(Mode mode)
{
    switch (mode) {
    case DustOnTransparency:
        return Erase;
    case DustOnWhite:
        return PaintWhite;
    case TransparentGaps:
        return SurroundingColor;
    }
    return Erase;
}

QVector<KisDustDetector::Component>
KisDustDetector::detect(KisPaintDeviceSP reference, const QRect &rect, const Options &options, KoUpdater *progress)
{
    return detect(reference, rect, options, nullptr, FullyInside, progress);
}

QVector<KisDustDetector::Component> KisDustDetector::detect(KisPaintDeviceSP reference,
                                                            const QRect &rect,
                                                            const Options &options,
                                                            KisPixelSelectionSP enclosure,
                                                            Containment containment,
                                                            KoUpdater *progress)
{
    QVector<Component> result;
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(reference, result);

    if (rect.isEmpty()) {
        return result;
    }

    const int dustSize = qBound(minimumDustSize, options.dustSize, maximumDustSize);
    const QRect window = neededRect(rect, dustSize);
    const int width = window.width();
    const int height = window.height();
    const auto indexOf = [&](int x, int y) {
        return size_t(y - window.y()) * size_t(width) + size_t(x - window.x());
    };

    const auto cancelled = [progress](int percent) {
        if (!progress) {
            return false;
        }
        progress->setProgress(percent);
        return progress->interrupted();
    };

#ifdef HAVE_KRITA_GPU_ENGINE
    // Download stale tiles in batches instead of one by one while iterating.
    KisGpuTileAccess::syncToCpu(reference, window);
#endif

    const KoColorSpace *cs = reference->colorSpace();
    std::vector<quint8> state(size_t(width) * size_t(height), Background);
    {
        Classifier classifier(cs, options);
        KisSequentialConstIterator it(reference, window);
        while (it.nextPixel()) {
            if (it.x() == window.x() && it.y() % 64 == 0 && cancelled(40 * (it.y() - window.y()) / height)) {
                return QVector<Component>();
            }
            if (classifier.isCandidate(it.oldRawData())) {
                state[indexOf(it.x(), it.y())] = Candidate;
            }
        }
    }

    std::vector<quint8> enclosed;
    if (enclosure) {
        enclosed.resize(state.size());
        enclosure->readBytes(enclosed.data(), window);
    }

    KisRandomConstAccessorSP accessor = reference->createRandomConstAccessorNG();
    const int pixelSize = cs->pixelSize();

    std::vector<QPoint> stack;
    QVector<QPoint> pixels;
    QVector<QPoint> surroundings;

    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        if (y % 16 == 0 && cancelled(40 + 59 * (y - rect.y()) / rect.height())) {
            return QVector<Component>();
        }

        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (state[indexOf(x, y)] != Candidate) {
                continue;
            }

            // Flood the whole component so each pixel is visited once. Once it
            // is too large, stop collecting its pixels but keep marking them.
            bool isDust = true;
            QRect bounds(x, y, 1, 1);
            int insideCount = 0;
            pixels.clear();
            stack.clear();
            stack.push_back(QPoint(x, y));
            state[indexOf(x, y)] = Visited;

            while (!stack.empty()) {
                const QPoint p = stack.back();
                stack.pop_back();

                if (isDust) {
                    bounds |= QRect(p, QSize(1, 1));
                    if (bounds.width() > dustSize || bounds.height() > dustSize) {
                        isDust = false;
                        pixels.clear();
                    } else {
                        pixels.append(p);
                        if (enclosure && enclosed[indexOf(p.x(), p.y())] >= enclosureThreshold) {
                            ++insideCount;
                        }
                    }
                }

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = p.x() + dx;
                        const int ny = p.y() + dy;
                        if (!window.contains(nx, ny)) {
                            // The component may continue outside the window,
                            // so its size is unknown.
                            if (isDust) {
                                isDust = false;
                                pixels.clear();
                            }
                            continue;
                        }
                        quint8 &s = state[indexOf(nx, ny)];
                        if (s == Candidate) {
                            s = Visited;
                            stack.push_back(QPoint(nx, ny));
                        }
                    }
                }
            }

            if (!isDust) {
                continue;
            }

            if (enclosure) {
                const bool accepted = containment == FullyInside ? insideCount == pixels.size() : insideCount > 0;
                if (!accepted) {
                    continue;
                }
            }

            Component component;
            component.bounds = bounds;
            component.pixels = pixels;

            if (options.mode == TransparentGaps) {
                // A dust component never touches the window edge, so all its
                // neighbors are inside the window.
                surroundings.clear();
                for (const QPoint &p : std::as_const(pixels)) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            quint8 &s = state[indexOf(p.x() + dx, p.y() + dy)];
                            if (s == Background) {
                                s = Surrounding;
                                surroundings.append(QPoint(p.x() + dx, p.y() + dy));
                            }
                        }
                    }
                }
                for (const QPoint &p : std::as_const(surroundings)) {
                    state[indexOf(p.x(), p.y())] = Background;
                }

                QByteArray colors(surroundings.size() * pixelSize, '\0');
                for (int i = 0; i < surroundings.size(); ++i) {
                    accessor->moveTo(surroundings[i].x(), surroundings[i].y());
                    std::memcpy(colors.data() + i * pixelSize, accessor->oldRawData(), pixelSize);
                }
                component.surroundingColor = mainSurroundingColor(cs, colors, surroundings.size());
            }

            result.append(component);
        }
    }

    if (progress) {
        progress->setProgress(100);
    }

    return result;
}

void KisDustDetector::apply(KisPaintDeviceSP target,
                            const QVector<Component> &components,
                            Replacement replacement,
                            const KoColor &fixedColor,
                            const QRect &clipRect)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(target);

    const KoColorSpace *cs = target->colorSpace();
    const int pixelSize = cs->pixelSize();

    KoColor color;
    switch (replacement) {
    case Erase:
        color = KoColor::createTransparent(cs);
        break;
    case PaintWhite:
        color = KoColor(Qt::white, cs);
        break;
    case FixedColor:
        color = fixedColor.convertedTo(cs);
        break;
    case SurroundingColor:
        break;
    }

#ifdef HAVE_KRITA_GPU_ENGINE
    QRect bounds;
    for (const Component &component : components) {
        bounds |= component.bounds;
    }
    if (!clipRect.isEmpty()) {
        bounds &= clipRect;
    }
    if (!bounds.isEmpty()) {
        KisGpuTileAccess::syncToCpu(target, bounds);
    }
#endif

    KisRandomAccessorSP accessor = target->createRandomAccessorNG();

    for (const Component &component : components) {
        if (!clipRect.isEmpty() && !clipRect.intersects(component.bounds)) {
            continue;
        }

        if (replacement == SurroundingColor) {
            color = component.surroundingColor.convertedTo(cs);
        }

        for (const QPoint &p : component.pixels) {
            if (!clipRect.isEmpty() && !clipRect.contains(p)) {
                continue;
            }
            accessor->moveTo(p.x(), p.y());
            std::memcpy(accessor->rawData(), color.data(), pixelSize);
        }
    }
}

void KisDustDetector::select(KisPixelSelectionSP selection, const QVector<Component> &components)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(selection);

    KisRandomAccessorSP accessor = selection->createRandomAccessorNG();
    for (const Component &component : components) {
        for (const QPoint &p : component.pixels) {
            accessor->moveTo(p.x(), p.y());
            *accessor->rawData() = MAX_SELECTED;
        }
    }
}
