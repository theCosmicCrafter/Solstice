/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QDataStream>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>

#include "KisBrushTestMain.h"

#include <KisAbrPresetConverter.h>
#include <KisAbrStorage.h>
#include <KisAsynchronousStrokeUpdateHelper.h>
#include <KisLocalStrokeResources.h>
#include <KisSizeOptionData.h>
#include <KisStandardOptionData.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_abr_brush_collection.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_resources_snapshot.h>
#include <kis_undo_stores.h>
#include <strokes/KisFreehandStrokeInfo.h>
#include <strokes/freehand_stroke.h>

/**
 * ABR import (docs/agent/abr-import.md): an ABR file's brush presets
 * become Pixel Brush presets that paint with the file's tips.
 */
class KisAbrPresetTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testPresetsFromBundledFile();
    void testPresetsPaint();
    void testReload();
    void testFolders();
    void testFolderForms();
    void testColorDynamicsAndScatterCount();
    void testTextureDepthModes();
};

namespace
{
QString bundledFile()
{
    return QString(FILES_DATA_DIR) + "/../../../../../../libs/brush/tests/data/brushes_by_mar_ka_d338ela.abr";
}

/// the preset with the file's tips and patterns as its resources, as they
/// would be in the resource database
KisPaintOpPresetSP withFileResources(KisPaintOpPresetSP preset, const KisAbrBrushCollection &collection)
{
    QList<KoResourceSP> resources;
    for (KisAbrBrushSP tip : collection.brushes()) {
        resources << tip;
    }
    for (KoPatternSP pattern : collection.patternsMap()->values()) {
        resources << pattern;
    }
    KisPaintOpPresetSP copy = preset->clone().dynamicCast<KisPaintOpPreset>();
    copy->setResourcesInterface(QSharedPointer<KisLocalStrokeResources>::create(resources));
    return copy;
}

QImage
paintedImage(KisPaintOpPresetSP preset, const QColor &foreground = Qt::black, const QColor &background = Qt::white);

int paintedPixels(KisPaintOpPresetSP preset)
{
    const QImage result = paintedImage(preset);
    int painted = 0;
    for (int y = 0; y < result.height(); y++) {
        for (int x = 0; x < result.width(); x++) {
            painted += qAlpha(result.pixel(x, y)) > 20;
        }
    }
    return painted;
}

QImage paintedImage(KisPaintOpPresetSP preset, const QColor &foreground, const QColor &background)
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(new KisSurrogateUndoStore(), 300, 200, cs, "abr");
    KisPaintLayerSP layer = new KisPaintLayer(image, "layer", OPACITY_OPAQUE_U8, cs);
    image->addNode(layer, image->root());

    const KoColor color(foreground, cs);
    KoCanvasResourceProvider provider;
    provider.setResource(KoCanvasResource::ForegroundColor, QVariant::fromValue(color));
    provider.setResource(KoCanvasResource::BackgroundColor, QVariant::fromValue(KoColor(background, cs)));
    provider.setResource(KoCanvasResource::CurrentPaintOpPreset, QVariant::fromValue(preset));
    provider.setResource(KoCanvasResource::Opacity, 1.0);
    provider.setResource(KoCanvasResource::CurrentCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::CurrentEffectiveCompositeOp, COMPOSITE_OVER);
    provider.setResource(KoCanvasResource::EffectiveZoom, 1.0);
    provider.setResource(KoCanvasResource::HdrExposure, 0.0);
    provider.setResource(KoCanvasResource::EraserMode, false);
    provider.setResource(KoCanvasResource::GlobalAlphaLock, false);
    provider.setResource(KoCanvasResource::MirrorHorizontal, false);
    provider.setResource(KoCanvasResource::MirrorVertical, false);
    provider.setResource(KoCanvasResource::EffectiveLodAvailability, false);
    provider.setResource(KoCanvasResource::Size, preset->settings()->paintOpSize());

    KisResourcesSnapshotSP resources = new KisResourcesSnapshot(image, layer, &provider, nullptr, {}, preset);
    resources->setOpacity(1.0);
    resources->setFGColorOverride(color);
    FreehandStrokeStrategy *stroke =
        new FreehandStrokeStrategy(resources, new KisFreehandStrokeInfo(), kundo2_noi18n("abr stroke"));
    KisStrokeId id = image->startStroke(stroke);
    KisPaintInformation previous(QPointF(40, 100), 0.5);
    for (int i = 1; i <= 40; i++) {
        KisPaintInformation next(QPointF(40 + i * 5, 100 + 30 * qSin(i / 6.0)), 0.3 + 0.6 * qSin(i / 40.0 * M_PI));
        next.setCurrentTime(i * 10);
        image->addJob(id, new FreehandStrokeStrategy::Data(0, previous, next));
        previous = next;
    }
    image->addJob(id, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    image->endStroke(id);
    image->waitForDone();

    return layer->paintDevice()->convertToQImage(nullptr, QRect(0, 0, 300, 200));
}
} // namespace

void KisAbrPresetTest::testPresetsFromBundledFile()
{
    KisAbrBrushCollection collection(bundledFile());
    QVERIFY(collection.load());
    const auto presets = collection.presetsMap();
    qInfo() << "presets:" << presets->size();
    QVERIFY(presets->size() >= 30);

    int pressureSize = 0;
    int abrTips = 0;
    int autoTips = 0;
    int masked = 0;
    for (auto it = presets->constBegin(); it != presets->constEnd(); ++it) {
        KisPaintOpPresetSP preset = it.value();
        QVERIFY(it.key().startsWith("brushes_by_mar_ka_d338ela_preset_"));
        QVERIFY(it.key().endsWith(".kpp"));
        QVERIFY(!preset->name().isEmpty());
        QVERIFY(preset->valid());
        QVERIFY(!preset->image().isNull());
        KisPaintOpSettingsSP settings = preset->settings();
        QCOMPARE(settings->getString("paintop"), QString("paintbrush"));
        const QString tip = settings->getString("brush_definition");
        abrTips += tip.contains("abr_brush");
        autoTips += tip.contains("auto_brush");
        masked += settings->getBool("MaskingBrush/Enabled");

        // read back with the Pixel Brush's own option data
        KisSizeOptionData size;
        QVERIFY(size.read(settings.data()));
        if (size.isChecked && size.sensorStruct().sensorPressure.isActive) {
            pressureSize++;
        }
        KisOpacityOptionData opacity;
        QVERIFY(opacity.read(settings.data()));
        QVERIFY(opacity.strengthValue > 0.0 && opacity.strengthValue <= 1.0);
    }
    qInfo() << "ABR tips" << abrTips << "auto tips" << autoTips << "pressure size" << pressureSize << "masked"
            << masked;
    QVERIFY(abrTips > 0);
    QVERIFY(pressureSize > 0);
}

void KisAbrPresetTest::testPresetsPaint()
{
    KisAbrBrushCollection collection(bundledFile());
    QVERIFY(collection.load());
    int checked = 0;
    for (KisPaintOpPresetSP preset : collection.presetsMap()->values()) {
        const int painted = paintedPixels(withFileResources(preset, collection));
        if (painted <= 0) {
            qWarning() << "painted nothing:" << preset->name() << preset->settings()->getString("brush_definition");
        }
        QVERIFY2(painted > 0, qPrintable(preset->name()));
        if (++checked == 12) {
            break;
        }
    }
}

/// The storage hands out copies, so that editing a preset leaves the file's
/// version, and reloading the preset returns to that version
void KisAbrPresetTest::testReload()
{
    KisAbrStorage storage(bundledFile());
    auto presets = storage.resources(ResourceType::PaintOpPresets);
    QVERIFY(presets->hasNext());
    presets->next();
    const QString url = presets->url();

    KisPaintOpPresetSP edited = storage.resource(url).dynamicCast<KisPaintOpPreset>();
    QVERIFY(edited);
    KisPaintOpPresetSP again = storage.resource(url).dynamicCast<KisPaintOpPreset>();
    QVERIFY(edited.data() != again.data());
    const qreal opacity = edited->settings()->paintOpOpacity();
    const QString name = edited->name();

    edited->settings()->setPaintOpOpacity(opacity * 0.5);
    edited->setName(QStringLiteral("edited"));
    QCOMPARE(storage.resource(url)->name(), name);

    QVERIFY(storage.loadVersionedResource(edited));
    QCOMPARE(edited->name(), name);
    QCOMPARE(edited->settings()->paintOpOpacity(), opacity);
}

namespace
{
/// writes Photoshop descriptor items, big-endian
class DescriptorWriter
{
public:
    DescriptorWriter()
        : m_stream(&m_data, QIODevice::WriteOnly)
    {
        m_stream.setByteOrder(QDataStream::BigEndian);
    }

    void id(const QByteArray &id)
    {
        if (id.size() == 4) {
            m_stream << quint32(0);
        } else {
            m_stream << quint32(id.size());
        }
        m_stream.writeRawData(id.constData(), id.size());
    }

    void unicode(const QString &text)
    {
        m_stream << quint32(text.size() + 1);
        for (QChar c : text) {
            m_stream << quint16(c.unicode());
        }
        m_stream << quint16(0);
    }

    /// a descriptor's name, class and number of items
    void descriptor(const QByteArray &classId, int items)
    {
        unicode(QString());
        id(classId);
        m_stream << quint32(items);
    }

    void key(const QByteArray &key, const char *osType)
    {
        id(key);
        m_stream.writeRawData(osType, 4);
    }

    void text(const QByteArray &key, const QString &value)
    {
        this->key(key, "TEXT");
        unicode(value);
    }

    void raw(const char *data, int size)
    {
        m_stream.writeRawData(data, size);
    }

    void count(int n)
    {
        m_stream << quint32(n);
    }

    QByteArray data() const
    {
        return m_data;
    }

private:
    QByteArray m_data;
    QDataStream m_stream;
};

/// a `phry` section body: its version, then a descriptor with the
/// `hierarchy` list of tokens; "+Name" opens a folder, "-" ends one, and
/// "p" is a preset
QByteArray hierarchySection(const QStringList &tokens)
{
    DescriptorWriter w;
    w.count(16);
    w.descriptor("null", 1);
    w.key("hierarchy", "VlLs");
    w.count(tokens.size());
    for (const QString &token : tokens) {
        w.raw("Objc", 4);
        if (token.startsWith('+')) {
            w.descriptor("Grup", 2);
            w.text("Nm  ", token.mid(1));
            w.text("zuid", QStringLiteral("id-") + token.mid(1));
        } else if (token == QStringLiteral("-")) {
            w.descriptor("groupEnd", 0);
        } else {
            w.descriptor("preset", 0);
        }
    }
    QByteArray result;
    QDataStream s(&result, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::BigEndian);
    const QByteArray body = w.data();
    s.writeRawData("8BIMphry", 8);
    s << quint32(body.size());
    s.writeRawData(body.constData(), body.size());
    while (result.size() % 4) {
        result += '\0';
    }
    return result;
}

QStringList tagResources(KisAbrStorage &storage, const QString &url, QString *name)
{
    auto tags = storage.tags(ResourceType::PaintOpPresets);
    while (tags->hasNext()) {
        tags->next();
        if (tags->tag()->url() == url) {
            *name = tags->tag()->name();
            QStringList resources = tags->tag()->defaultResources();
            resources.sort();
            return resources;
        }
    }
    return {};
}
} // namespace

/// Phase 5: the folders of Photoshop's Brushes panel (the `phry` section)
/// become tags of the presets
void KisAbrPresetTest::testFolders()
{
    QFile original(bundledFile());
    QVERIFY(original.open(QIODevice::ReadOnly));
    QByteArray data = original.readAll();
    while (data.size() % 4) {
        data += '\0';
    }
    // a stray end is ignored and the last folder is left open
    data += hierarchySection({"p", "+Ink", "p", "p", "+Wet", "p", "-", "-", "p", "-", "+Open", "p"});

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("folders.abr"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
    }

    KisAbrBrushCollection collection(path);
    QVERIFY(collection.load());
    for (int i = 1; i <= 6; i++) {
        QVERIFY(collection.presetsMap()->contains(QStringLiteral("folders_preset_%1.kpp").arg(i)));
    }
    const auto folders = collection.presetFolders();
    QCOMPARE(folders.size(), 4);
    QCOMPARE(folders.value("folders_preset_2.kpp"), QStringList({"Ink"}));
    QCOMPARE(folders.value("folders_preset_3.kpp"), QStringList({"Ink"}));
    QCOMPARE(folders.value("folders_preset_4.kpp"), QStringList({"Ink", "Wet"}));
    QCOMPARE(folders.value("folders_preset_6.kpp"), QStringList({"Open"}));

    KisAbrStorage storage(path);
    QString name;
    QCOMPARE(tagResources(storage, "folders.abr/Ink", &name),
             QStringList({"folders_preset_2.kpp", "folders_preset_3.kpp", "folders_preset_4.kpp"}));
    QCOMPARE(name, QString("folders / Ink"));
    QCOMPARE(tagResources(storage, "folders.abr/Ink/Wet", &name), QStringList({"folders_preset_4.kpp"}));
    QCOMPARE(name, QString("folders / Ink / Wet"));
    QCOMPARE(tagResources(storage, "folders.abr/Open", &name), QStringList({"folders_preset_6.kpp"}));
    // the file's own tag still has all the presets
    QCOMPARE(tagResources(storage, "folders.abr", &name).size(), collection.presetsMap()->size());

    // without folders, only the file's tag
    KisAbrStorage plain(bundledFile());
    auto tags = plain.tags(ResourceType::PaintOpPresets);
    int count = 0;
    while (tags->hasNext()) {
        tags->next();
        count++;
    }
    QCOMPARE(count, 1);
}

/// The folder tokens as plain descriptors with one item, and a folder that
/// holds its contents as a list
void KisAbrPresetTest::testFolderForms()
{
    QDomDocument doc;
    QVERIFY(doc.setContent(QStringLiteral(
        "<asl><node type='Descriptor' classId='null'><node key='hierarchy' type='List'>"
        "<node type='Descriptor' classId='null'><node key='Grup' type='Descriptor' classId='Grup'>"
        "<node key='Nm  ' type='Text' value='Dry'/></node></node>"
        "<node type='Descriptor' classId='null'><node key='preset' type='Descriptor' classId='null'/></node>"
        "<node type='Descriptor' classId='null'><node key='groupEnd' type='Descriptor' classId='null'/></node>"
        "<node type='Descriptor' classId='Grup'><node key='Nm  ' type='Text' value='Nested'/>"
        "<node key='Kids' type='List'><node type='Descriptor' classId='preset'/></node></node>"
        "<node type='Descriptor' classId='preset'/>"
        "</node></node></asl>")));
    const QVector<QStringList> folders = KisAbrPresetConverter::presetFolders(doc.documentElement());
    QCOMPARE(folders.size(), 3);
    QCOMPARE(folders[0], QStringList({"Dry"}));
    QCOMPARE(folders[1], QStringList({"Nested"}));
    QCOMPARE(folders[2], QStringList());

    // deeper than 32 folders: the deeper ones are left out
    QStringList deep;
    for (int i = 0; i < 40; i++) {
        deep << QStringLiteral(
                    "<node type='Descriptor' classId='Grup'><node key='Nm  ' type='Text' value='%1'/></node>")
                    .arg(i);
    }
    QDomDocument deepDoc;
    QVERIFY(deepDoc.setContent(QStringLiteral("<asl><node key='hierarchy' type='List'>%1"
                                              "<node type='Descriptor' classId='preset'/></node></asl>")
                                   .arg(deep.join(QString()))));
    const QVector<QStringList> deepFolders = KisAbrPresetConverter::presetFolders(deepDoc.documentElement());
    QCOMPARE(deepFolders.size(), 1);
    QCOMPARE(deepFolders[0].size(), 32);
}

namespace
{
/// a Photoshop preset with a round tip, as the descriptor reader gives it
QString roundPresetXml(const QString &extra)
{
    return QStringLiteral(
               "<asl><node type='Descriptor' classId='null'><node key='Brsh' type='List'>"
               "<node type='Descriptor' classId='brushPreset'>"
               "<node key='Nm  ' type='Text' value='Round'/>"
               "<node key='Brsh' type='Descriptor' classId='computedBrush'>"
               "<node key='Dmtr' type='UnitFloat' unit='#Pxl' value='20'/>"
               "<node key='Hrdn' type='UnitFloat' unit='#Prc' value='100'/>"
               "<node key='Spcn' type='UnitFloat' unit='#Prc' value='25'/></node>"
               "%1</node></node></node></asl>")
        .arg(extra);
}

KisPaintOpPresetSP convertOne(const QString &xml)
{
    QDomDocument doc;
    if (!doc.setContent(xml)) {
        return KisPaintOpPresetSP();
    }
    KisAbrPresetConverter::Sources sources;
    sources.baseName = QStringLiteral("round");
    const auto result = KisAbrPresetConverter::convert(doc.documentElement(), sources);
    return result.presets.value(QStringLiteral("round_preset_1.kpp"));
}

int distinctOpaqueColors(const QImage &image)
{
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y++) {
        for (int x = 0; x < image.width(); x++) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) > 200) {
                colors.insert(pixel | 0xff000000);
            }
        }
    }
    return colors.size();
}

qreal tipSpacing(KisPaintOpPresetSP preset)
{
    QDomDocument doc;
    doc.setContent(preset->settings()->getString("brush_definition"));
    return doc.documentElement().attribute("spacing").toDouble();
}
} // namespace

/// Color dynamics become the Mix and HSV options; the scatter count brings
/// the dabs closer together
void KisAbrPresetTest::testColorDynamicsAndScatterCount()
{
    const KisPaintOpPresetSP plain = convertOne(roundPresetXml(QString()));
    QVERIFY(plain);
    QCOMPARE(tipSpacing(plain), 0.25);
    KisMixOptionData plainMix;
    QVERIFY(plainMix.read(plain->settings().data()));
    QVERIFY(!plainMix.isChecked);
    QCOMPARE(distinctOpaqueColors(paintedImage(plain, Qt::red, Qt::blue)), 1);

    const QString dynamics = QStringLiteral(
        "<node key='useScatter' type='Boolean' value='1'/>"
        "<node key='Cnt ' type='Integer' value='4'/>"
        "<node key='useColorDynamics' type='Boolean' value='1'/>"
        "<node key='clVr' type='Descriptor' classId='brVr'>"
        "<node key='bVTy' type='Integer' value='0'/>"
        "<node key='jitter' type='UnitFloat' unit='#Prc' value='100'/></node>"
        "<node key='H   ' type='UnitFloat' unit='#Prc' value='20'/>"
        "<node key='Strt' type='UnitFloat' unit='#Prc' value='30'/>"
        "<node key='Brgh' type='UnitFloat' unit='#Prc' value='10'/>"
        "<node key='purity' type='UnitFloat' unit='#Prc' value='-40'/>"
        "<node key='colorDynamicsPerTip' type='Boolean' value='%1'/>");

    const KisPaintOpPresetSP perStroke = convertOne(roundPresetXml(dynamics.arg(0)));
    QVERIFY(perStroke);
    QCOMPARE(tipSpacing(perStroke), 0.0625);
    KisMixOptionData mix;
    QVERIFY(mix.read(perStroke->settings().data()));
    QVERIFY(mix.isChecked);
    QVERIFY(mix.sensorStruct().sensorFuzzyPerStroke.isActive);
    KisHueOptionData hue;
    QVERIFY(hue.read(perStroke->settings().data()));
    QVERIFY(hue.isChecked);
    QCOMPARE(hue.strengthValue, 0.2);
    KisSaturationOptionData saturation;
    QVERIFY(saturation.read(perStroke->settings().data()));
    QVERIFY(saturation.isChecked);
    // purity -40 and jitter 30: from -70% to -10%
    QCOMPARE(saturation.sensorStruct().sensorFuzzyPerStroke.curve, QString("0,0.15;1,0.45;"));
    KisValueOptionData value;
    QVERIFY(value.read(perStroke->settings().data()));
    QVERIFY(value.isChecked);

    // per tip: the dabs of one stroke take many colors
    const KisPaintOpPresetSP perTip = convertOne(roundPresetXml(dynamics.arg(1)));
    QVERIFY(perTip);
    KisMixOptionData tipMix;
    QVERIFY(tipMix.read(perTip->settings().data()));
    QVERIFY(tipMix.sensorStruct().sensorFuzzyPerDab.isActive);
    const int colors = distinctOpaqueColors(paintedImage(perTip, Qt::red, Qt::blue));
    qInfo() << "colors with per-tip color dynamics" << colors;
    QVERIFY(colors > 20);

    // purity -100 alone takes the color out
    const KisPaintOpPresetSP gray =
        convertOne(roundPresetXml(QStringLiteral("<node key='useColorDynamics' type='Boolean' value='1'/>"
                                                 "<node key='purity' type='UnitFloat' unit='#Prc' value='-100'/>")));
    QVERIFY(gray);
    const QImage grayImage = paintedImage(gray, Qt::red, Qt::blue);
    bool painted = false;
    for (int y = 0; y < grayImage.height(); y++) {
        for (int x = 0; x < grayImage.width(); x++) {
            const QRgb pixel = grayImage.pixel(x, y);
            if (qAlpha(pixel) > 200) {
                painted = true;
                QVERIFY2(qAbs(qRed(pixel) - qGreen(pixel)) < 16 && qAbs(qRed(pixel) - qBlue(pixel)) < 16,
                         qPrintable(QColor(pixel).name()));
            }
        }
    }
    QVERIFY(painted);
}

/// Photoshop's Height mode at a shallow depth paints where the pattern is
/// dark; brushes darken the pattern to let paint through. Photoshop
/// inverts the pattern before its brightness, Solstice after.
void KisAbrPresetTest::testTextureDepthModes()
{
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(QColor(150, 150, 150));
    KoPatternSP pattern(new KoPattern(image, QStringLiteral("Gray"), QStringLiteral("gray.pat")));

    auto convert = [&](bool invert) {
        QDomDocument doc;
        doc.setContent(roundPresetXml(QStringLiteral("<node key='useTexture' type='Boolean' value='1'/>"
                                                     "<node key='Txtr' type='Descriptor' classId='Ptrn'>"
                                                     "<node key='Idnt' type='Text' value='gray'/></node>"
                                                     "<node key='textureBlendMode' type='Enum' value='Hght'/>"
                                                     "<node key='textureDepth' type='UnitFloat' value='9'/>"
                                                     "<node key='textureBrightness' type='Integer' value='4294967196'/>"
                                                     "<node key='InvT' type='Boolean' value='%1'/>")
                                          .arg(invert ? 1 : 0)));
        KisAbrPresetConverter::Sources sources;
        sources.baseName = QStringLiteral("round");
        sources.patternsById.insert(QStringLiteral("gray"), pattern);
        KisPaintOpPresetSP preset =
            KisAbrPresetConverter::convert(doc.documentElement(), sources).presets.value("round_preset_1.kpp");
        if (preset) {
            preset->setResourcesInterface(
                QSharedPointer<KisLocalStrokeResources>::create(QList<KoResourceSP>({pattern})));
        }
        return preset;
    };

    const KisPaintOpPresetSP plain = convert(false);
    QVERIFY(plain);
    QCOMPARE(plain->settings()->getInt("Texture/Pattern/TexturingMode"), 14);
    QVERIFY(!plain->settings()->getBool("Texture/Pattern/Invert"));
    // brightness -100: darker, which Solstice subtracts as a positive value
    QVERIFY(qAbs(plain->settings()->getDouble("Texture/Pattern/Brightness") - 100.0 / 150.0) < 1e-6);
    const int plainPixels = paintedPixels(plain);
    qInfo() << "height mode pixels" << plainPixels;
    QVERIFY(plainPixels > 500);

    const KisPaintOpPresetSP inverted = convert(true);
    QVERIFY(inverted);
    QVERIFY(inverted->settings()->getBool("Texture/Pattern/Invert"));
    QVERIFY(qAbs(inverted->settings()->getDouble("Texture/Pattern/Brightness") + 100.0 / 150.0) < 1e-6);
    QVERIFY(paintedPixels(inverted) > 500);
}

SOLSTICE_BRUSH_TEST_MAIN_WITH_BUNDLES(KisAbrPresetTest, QStringLiteral("Krita_4_Default_Resources.bundle"))

#include "KisAbrPresetTest.moc"
