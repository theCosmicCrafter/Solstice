/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../KisBrushOpSettings.h"
#include "../kis_brushop.h"

#include <KisFakeRunnableStrokeJobsExecutor.h>
#include <KisLocalStrokeResources.h>
#include <KisRunnableStrokeJobData.h>
#include <KisSizeOptionData.h>
#include <KisStandardOptionData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <brushengine/KisMinimumDabSize.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop_preset.h>
#include <kis_config.h>
#include <kis_default_bounds.h>
#include <kis_distance_information.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <kis_pointer_utils.h>
#include <kis_resources_snapshot.h>
#include <simpletest.h>

#include <KSharedConfig>
#include <QScopeGuard>

/**
 * "At least 1 pixel" (docs/agent/brush-minimum-one-pixel.md): a brush-based
 * paint op raises a dab smaller than 1 px to 1 px when the stroke's preset
 * carries KisMinimumDabSize::settingsKey(), and paints as before without it.
 */
class KisBrushMinimumOnePixelTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testApply();
    void testLowPressureStroke_data();
    void testLowPressureStroke();
    void testMatchesOnePixelBrush_data();
    void testMatchesOnePixelBrush();
    void testSnapshotCarriesToolOption();
};

namespace
{
class TestBounds : public KisDefaultBounds
{
public:
    TestBounds(int lod)
        : m_lod(lod)
    {
    }
    QRect bounds() const override
    {
        return QRect(0, 0, 256, 128);
    }
    int currentLevelOfDetail() const override
    {
        return m_lod;
    }

private:
    int m_lod;
};

class BrushOp : public KisBrushOp
{
public:
    using KisBrushOp::KisBrushOp;
};

/// A Pixel Brush with a round auto tip of @p diameter px whose size follows
/// pressure linearly; opacity and flow are fixed.
KisPaintOpSettingsSP brushSettings(qreal diameter, std::optional<bool> minimumOnePixel)
{
    KisPaintOpSettingsSP settings = new KisBrushOpSettings(toQShared(new KisLocalStrokeResources()));
    settings->setProperty("paintop", "paintbrush");
    settings->setProperty("brush_definition",
                          QStringLiteral("<Brush useAutoSpacing=\"0\" angle=\"0\" spacing=\"0.1\" density=\"1\" "
                                         "BrushVersion=\"2\" type=\"auto_brush\" randomness=\"0\"><MaskGenerator "
                                         "spikes=\"2\" hfade=\"1\" ratio=\"1\" diameter=\"%1\" id=\"default\" "
                                         "type=\"circle\" antialiasEdges=\"1\" vfade=\"1\"/></Brush>")
                              .arg(diameter));
    KisSizeOptionData size;
    size.isChecked = true;
    size.useCurve = true;
    size.write(settings.data());
    KisOpacityOptionData opacity;
    opacity.useCurve = false;
    opacity.write(settings.data());
    KisFlowOptionData flow;
    flow.useCurve = false;
    flow.write(settings.data());
    if (minimumOnePixel) {
        settings->setProperty(KisMinimumDabSize::settingsKey(), *minimumOnePixel);
    }
    return settings;
}

/// Paints a horizontal line from x 20 to 220 at y 64 (level of detail 0
/// coordinates) with a constant @p pressure.
KisPaintDeviceSP paintLine(KisPaintOpSettingsSP settings, qreal pressure, int lod = 0)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisPaintDeviceSP device = new KisPaintDevice(cs);
    device->setDefaultBounds(new TestBounds(lod));

    KisFakeRunnableStrokeJobsExecutor executor(KisFakeRunnableStrokeJobsExecutor::AllowBarrierJobs);
    KisPainter painter(device);
    painter.setRunnableStrokeJobsInterface(&executor);
    painter.setPaintColor(KoColor(Qt::black, cs));
    {
        BrushOp op(settings, &painter, KisNodeSP(), KisImageSP());
        const qreal lodScale = 1.0 / (1 << lod);
        // a fractional y keeps the line between pixel rows
        const KisPaintInformation start(QPointF(20.0, 64.3) * lodScale, pressure);
        const KisPaintInformation end(QPointF(220.0, 64.3) * lodScale, pressure);
        KisDistanceInformation distance;
        // KisBrushOp::paintAt(info) hides the public overload
        static_cast<KisPaintOp &>(op).paintAt(start, &distance);
        op.paintLine(start, end, &distance);

        bool needsMore = true;
        for (int i = 0; needsMore && i < 1000; ++i) {
            QVector<KisRunnableStrokeJobData *> jobs;
            needsMore = op.doAsynchronousUpdate(jobs).second;
            executor.addRunnableJobs(implicitCastList<KisRunnableStrokeJobDataBase *>(jobs));
        }
        if (needsMore) {
            qWarning() << "the brush op kept asking for updates";
            return KisPaintDeviceSP();
        }
    }
    painter.setRunnableStrokeJobsInterface(nullptr);
    return device;
}

/// Sum of the alpha of the pixels of each column of @p rect, 0.0-1.0 each.
QVector<qreal> columnCoverage(KisPaintDeviceSP device, const QRect &rect)
{
    QVector<qreal> coverage(rect.width(), 0.0);
    const int pixelSize = device->pixelSize();
    QByteArray bytes(rect.width() * rect.height() * pixelSize, '\0');
    device->readBytes(reinterpret_cast<quint8 *>(bytes.data()), rect);
    const KoColorSpace *cs = device->colorSpace();
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            const quint8 *pixel =
                reinterpret_cast<const quint8 *>(bytes.constData()) + (y * rect.width() + x) * pixelSize;
            coverage[x] += cs->opacityF(pixel);
        }
    }
    return coverage;
}

QByteArray pixels(KisPaintDeviceSP device)
{
    if (!device) {
        return QByteArray("no device");
    }
    const QRect rect = device->exactBounds();
    QByteArray bytes(rect.width() * rect.height() * device->pixelSize(), '\0');
    device->readBytes(reinterpret_cast<quint8 *>(bytes.data()), rect);
    return QByteArray::number(rect.x()) + ',' + QByteArray::number(rect.y()) + ',' + QByteArray::number(rect.width())
        + ',' + QByteArray::number(rect.height()) + ':' + bytes;
}
} // namespace

void KisBrushMinimumOnePixelTest::testApply()
{
    // a 10 px tip at 2 % pressure is 0.2 px: raised to 1 px
    QCOMPARE(KisMinimumDabSize::apply(0.02, 10.0), 0.1);
    QCOMPARE(KisMinimumDabSize::apply(0.0, 10.0), 0.1);
    // larger dabs are unchanged
    QCOMPARE(KisMinimumDabSize::apply(0.5, 10.0), 0.5);
    QCOMPARE(KisMinimumDabSize::apply(0.1, 10.0), 0.1);
    // a tip smaller than 1 px is drawn at 1 px too
    QCOMPARE(KisMinimumDabSize::apply(1.0, 0.5), 2.0);
    // no tip size: nothing to do
    QCOMPARE(KisMinimumDabSize::apply(0.02, 0.0), 0.02);
}

void KisBrushMinimumOnePixelTest::testLowPressureStroke_data()
{
    QTest::addColumn<qreal>("pressure");
    QTest::newRow("zero") << 0.0;
    QTest::newRow("near zero") << 0.02;
}

void KisBrushMinimumOnePixelTest::testLowPressureStroke()
{
    QFETCH(qreal, pressure);

    // 10 px tip: 0.0 or 0.2 px at this pressure
    const KisPaintDeviceSP on = paintLine(brushSettings(10.0, true), pressure);
    const KisPaintDeviceSP off = paintLine(brushSettings(10.0, false), pressure);
    const KisPaintDeviceSP absent = paintLine(brushSettings(10.0, std::nullopt), pressure);
    // what a 1 px brush draws at full pressure
    const KisPaintDeviceSP reference = paintLine(brushSettings(1.0, false), 1.0);
    QVERIFY(on && off && absent && reference);

    // without the property the paint op behaves as before the option
    QCOMPARE(pixels(absent), pixels(off));

    // inside the line, away from its round ends
    const QRect line(40, 54, 160, 21);
    const QVector<qreal> onCoverage = columnCoverage(on, line);
    const QVector<qreal> offCoverage = columnCoverage(off, line);
    const QVector<qreal> referenceCoverage = columnCoverage(reference, line);
    qreal onMin = 1e9;
    qreal onMax = 0.0;
    qreal offMax = 0.0;
    qreal referenceMin = 1e9;
    for (int x = 0; x < line.width(); ++x) {
        onMin = qMin(onMin, onCoverage[x]);
        onMax = qMax(onMax, onCoverage[x]);
        offMax = qMax(offMax, offCoverage[x]);
        referenceMin = qMin(referenceMin, referenceCoverage[x]);
    }
    qInfo() << "pressure" << pressure << "column coverage: on" << onMin << "-" << onMax << "off, maximum" << offMax
            << "1 px brush, minimum" << referenceMin;

    // ON: every column holds as much ink as the line of a 1 px brush (an
    // antialiased 1 px dab covers less than a whole pixel)
    QVERIFY2(onMin >= 0.95 * referenceMin, qPrintable(QString::number(onMin)));
    QVERIFY(onMin >= 0.5);
    // and the line stays about 1 px wide
    QVERIFY(onMax <= 1.5);
    if (pressure == 0.0) {
        // OFF: a zero size dab is skipped, nothing is painted
        QVERIFY(off->exactBounds().isEmpty());
    } else {
        QVERIFY2(offMax < 0.5 * onMin, qPrintable(QString::number(offMax)));
    }
}

void KisBrushMinimumOnePixelTest::testMatchesOnePixelBrush_data()
{
    QTest::addColumn<int>("lod");
    QTest::newRow("final") << 0;
    QTest::newRow("instant preview, level of detail 1") << 1;
    QTest::newRow("instant preview, level of detail 2") << 2;
}

void KisBrushMinimumOnePixelTest::testMatchesOnePixelBrush()
{
    QFETCH(int, lod);

    // A 1 px tip at full pressure and the same tip with the option at zero
    // pressure draw the same dabs at every level of detail: the minimum
    // applies before the level of detail scale, so an instant preview shows
    // the final line scaled down instead of a line 2^lod times as wide.
    const QByteArray full = pixels(paintLine(brushSettings(1.0, false), 1.0, lod));
    const QByteArray raised = pixels(paintLine(brushSettings(1.0, true), 0.0, lod));
    QCOMPARE(raised, full);

    // the same for a larger tip whose size at this pressure is below 1 px
    const QByteArray fullSize = pixels(paintLine(brushSettings(4.0, false), 0.25, lod));
    const QByteArray raisedSize = pixels(paintLine(brushSettings(4.0, true), 0.0, lod));
    QCOMPARE(raisedSize, fullSize);
}

void KisBrushMinimumOnePixelTest::testSnapshotCarriesToolOption()
{
    const QString key = KisMinimumDabSize::configKey();
    const bool previous = KisConfig(true).readEntry<bool>(key, KisMinimumDabSize::defaultEnabled());
    const auto restore = qScopeGuard([&]() {
        KisConfig(false).writeEntry<bool>(key, previous);
    });

    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(nullptr, 64, 64, cs, "minimum dab size");
    KisPaintLayerSP layer = new KisPaintLayer(image, "paint", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    KisPaintOpPresetSP preset(new KisPaintOpPreset());
    preset->setSettings(brushSettings(10.0, std::nullopt));
    preset->setName(QStringLiteral("minimum dab size"));
    KoCanvasResourceProvider canvasResources;
    canvasResources.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));

    Q_FOREACH (const bool enabled, QList<bool>({true, false})) {
        KisConfig(false).writeEntry<bool>(key, enabled);
        KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &canvasResources);
        KisPaintOpSettingsSP strokeSettings = resources->currentPaintOpPreset()->settings();
        QVERIFY(strokeSettings->hasProperty(KisMinimumDabSize::settingsKey()));
        QCOMPARE(strokeSettings->getBool(KisMinimumDabSize::settingsKey(), !enabled), enabled);
        // only the stroke's clone carries it, not the user's preset
        QVERIFY(!preset->settings()->hasProperty(KisMinimumDabSize::settingsKey()));
        QVERIFY(!preset->isDirty());
    }

    // a preset passed in (Brush Stroke Layer replay, stroke previews) keeps
    // what it carries: an absent value stays absent (option off)
    KisConfig(false).writeEntry<bool>(key, true);
    KisResourcesSnapshotSP replay = new KisResourcesSnapshot(image, layer, &canvasResources, nullptr, {}, preset);
    QVERIFY(!replay->currentPaintOpPreset()->settings()->hasProperty(KisMinimumDabSize::settingsKey()));

    // a new profile has the option on
    KSharedConfig::openConfig()->group(QString()).deleteEntry(key);
    QVERIFY(KisConfig(true).readEntry<bool>(key, KisMinimumDabSize::defaultEnabled()));
}

SIMPLE_TEST_MAIN(KisBrushMinimumOnePixelTest)
#include "KisBrushMinimumOnePixelTest.moc"
