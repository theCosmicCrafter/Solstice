/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisColorReferenceResources.h"
#include <KisDocument.h>
#include <KisImportExportManager.h>
#include <KisPart.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorSpaceTraits.h>
#include <KoSegmentGradient.h>
#include <KoStopGradient.h>
#include <QColorSpace>
#include <QDataStream>
#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QSaveFile>
#include <cmath>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>
#include <memory>

namespace ColorReference
{
const KoColorSpace *workingSpace()
{
    return KoColorSpaceRegistry::instance()->colorSpace("RGBA", "F32", "sRGB-elle-V2-srgbtrc.icc");
}
double brightness(const RGB &c)
{
    return .2126 * c[0] + .7152 * c[1] + .0722 * c[2];
}
RGB lookup(const Table &table, double t)
{
    if (table.isEmpty())
        return {};
    const double x = qBound(0.0, t, 1.0) * (table.size() - 1);
    const int a = int(x), b = qMin(a + 1, int(table.size()) - 1);
    RGB result;
    for (int c = 0; c < 3; ++c)
        result[c] = table[a][c] + (x - a) * (table[b][c] - table[a][c]);
    return result;
}
RGB keepBrightness(RGB color, double target)
{
    const double l = brightness(color);
    double scale = 1;
    // Compress chroma around the requested luma, rather than clipping channels.
    // Outside SDR retain the unbounded luma and chroma; there is no SDR gamut.
    if (target >= 0 && target <= 1) {
        for (double v : color) {
            const double d = v - l;
            if (d > 0)
                scale = qMin(scale, (1 - target) / d);
            if (d < 0)
                scale = qMin(scale, -target / d);
        }
    }
    for (double &v : color)
        v = target + scale * (v - l);
    return color;
}
Table fromImage(const QImage &input)
{
    if (input.isNull())
        return {};
    QImage image = input.width() > 512 || input.height() > 512
        ? input.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : input;
    if (image.colorSpace().isValid())
        image.convertToColorSpace(QColorSpace::SRgb);
    image = image.convertToFormat(QImage::Format_ARGB32);
    std::array<RGB, 256> sums{};
    std::array<double, 256> weights{};
    double total = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = row[x];
            const double weight = qAlpha(pixel) / 255.0;
            const RGB color{qRed(pixel) / 255.0, qGreen(pixel) / 255.0, qBlue(pixel) / 255.0};
            const int bin = qBound(0, int(brightness(color) * 255), 255);
            weights[bin] += weight;
            total += weight;
            for (int c = 0; c < 3; ++c)
                sums[bin][c] += weight * color[c];
        }
    }
    if (!(total > 0))
        return {};
    QVector<double> positions;
    Table colors;
    double cumulative = 0;
    for (int i = 0; i < 256; ++i) {
        if (weights[i] == 0)
            continue;
        // Quantile midpoints retain the reference's tonal distribution. Absolute
        // brightness bins alone would make the keep-brightness switch redundant.
        positions.append((cumulative + .5 * weights[i]) / total);
        cumulative += weights[i];
        for (double &v : sums[i])
            v /= weights[i];
        colors.append(sums[i]);
    }
    Table result(TableSize);
    int upper = 0;
    for (int i = 0; i < TableSize; ++i) {
        const double t = double(i) / (TableSize - 1);
        while (upper + 1 < positions.size() && positions[upper] < t)
            ++upper;
        if (t <= positions.front())
            result[i] = colors.front();
        else if (t >= positions.back())
            result[i] = colors.back();
        else {
            const double f = (t - positions[upper - 1]) / (positions[upper] - positions[upper - 1]);
            for (int c = 0; c < 3; ++c)
                result[i][c] = colors[upper - 1][c] + f * (colors[upper][c] - colors[upper - 1][c]);
        }
    }
    return result;
}
Table fromGradient(KoAbstractGradientSP gradient)
{
    if (!gradient || !gradient->valid() || !workingSpace())
        return {};
    Table result(TableSize);
    KoColor color(workingSpace());
    for (int i = 0; i < TableSize; ++i) {
        gradient->colorAt(color, double(i) / (TableSize - 1));
        color.convertTo(workingSpace());
        const auto *p = reinterpret_cast<const KoRgbF32Traits::Pixel *>(color.data());
        result[i] = {p->red, p->green, p->blue};
    }
    return result;
}
QString encode(const Table &table)
{
    if (table.size() != TableSize)
        return {};
    QByteArray bytes("SCR1", 4);
    QDataStream stream(&bytes, QIODevice::Append);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (const auto &color : table)
        for (double v : color) {
            if (!std::isfinite(v) || std::abs(v) > 65504)
                return {};
            stream << float(v);
        }
    return QString::fromLatin1(bytes.toBase64());
}
Table decode(const QString &encoded)
{
    if (encoded.size() != ((4 + TableSize * 12 + 2) / 3) * 4)
        return {};
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (bytes.size() != 4 + TableSize * 12 || !bytes.startsWith("SCR1"))
        return {};
    QDataStream stream(bytes.mid(4));
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    Table result(TableSize);
    for (auto &color : result)
        for (double &v : color) {
            float f;
            stream >> f;
            if (!std::isfinite(f) || std::abs(f) > 65504)
                return {};
            v = f;
        }
    return stream.status() == QDataStream::Ok ? result : Table{};
}
QString gradientXml(KoAbstractGradientSP gradient)
{
    QDomDocument doc;
    auto element = doc.createElement("gradient");
    if (auto stop = gradient.dynamicCast<KoStopGradient>())
        stop->toXML(doc, element);
    else if (auto segment = gradient.dynamicCast<KoSegmentGradient>())
        segment->toXML(doc, element);
    else
        return {};
    element.setAttribute("name", gradient->name());
    doc.appendChild(element);
    return doc.toString();
}
KoAbstractGradientSP gradientFromXml(const QString &xml)
{
    QDomDocument doc;
    if (!doc.setContent(xml))
        return nullptr;
    const auto element = doc.documentElement();
    KoAbstractGradientSP gradient;
    if (element.attribute("type") == "stop")
        gradient = KoStopGradient::fromXML(element).clone().dynamicCast<KoAbstractGradient>();
    else if (element.attribute("type") == "segment")
        gradient = KoSegmentGradient::fromXML(element).clone().dynamicCast<KoAbstractGradient>();
    if (gradient) {
        gradient->setName(element.attribute("name"));
        gradient->setValid(true);
    }
    return gradient;
}
KoAbstractGradientSP defaultGradient()
{
    KoStopGradientSP gradient(new KoStopGradient());
    gradient->setStops(
        {KoGradientStop(0, KoColor(Qt::black, workingSpace())), KoGradientStop(1, KoColor(Qt::white, workingSpace()))});
    gradient->setName(i18n("Black to White"));
    gradient->setValid(true);
    return gradient;
}
QStringList presetNames()
{
    return {i18n("Amber"),
            i18n("Ocean"),
            i18n("Rose"),
            i18n("Forest"),
            i18n("Sunset"),
            i18n("Lavender"),
            i18n("Copper"),
            i18n("Moonlight")};
}
QImage presetImage(int index)
{
    // Original procedural palettes; no third-party artwork or preset assets.
    static const char *palettes[8][4] = {{"#332339", "#a85036", "#e4a654", "#fff0b8"},
                                         {"#101e52", "#205c96", "#36b1c1", "#d4f2e8"},
                                         {"#482247", "#a74181", "#eb8ca4", "#ffe3d0"},
                                         {"#172f35", "#366951", "#9ba85f", "#eee4ad"},
                                         {"#342664", "#a54879", "#ef794c", "#ffe28a"},
                                         {"#322d66", "#7770b7", "#bca1dd", "#f1dcf1"},
                                         {"#29252b", "#765340", "#bf8c5d", "#eddbb0"},
                                         {"#111d36", "#2c4769", "#647b99", "#bbc9d5"}};
    index = qBound(0, index, 7);
    QImage image(192, 128, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const double t = qBound(0.0, (x / 191.0 * .8 + y / 127.0 * .2), 1.0) * 3;
            const int a = qMin(2, int(t));
            const QColor left(palettes[index][a]), right(palettes[index][a + 1]);
            const double f = t - a;
            row[x] = qRgb(qRound(left.red() + f * (right.red() - left.red())),
                          qRound(left.green() + f * (right.green() - left.green())),
                          qRound(left.blue() + f * (right.blue() - left.blue())));
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
Library::Library(QString directory)
    : m_directory(std::move(directory))
{
}
QString Library::path(const QString &id) const
{
    if (id.isEmpty() || QFileInfo(id).fileName() != id || !id.endsWith(".png", Qt::CaseInsensitive))
        return {};
    const QString file = QDir(m_directory).filePath(id);
    return QFileInfo(file).isSymLink() ? QString() : file;
}
QStringList Library::entries() const
{
    return QDir(m_directory).entryList({"*.png"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
}
QImage Library::image(const QString &id) const
{
    const auto file = path(id);
    return file.isEmpty() ? QImage() : QImage(file);
}
QString Library::add(const QImage &input, const QString &name, QString *error)
{
    if (entries().size() >= 50) {
        *error = i18n("The library already contains 50 images.");
        return {};
    }
    if (input.isNull() || fromImage(input).isEmpty()) {
        *error = i18n("The image has no visible pixels.");
        return {};
    }
    if (!QDir().mkpath(m_directory)) {
        *error = i18n("Cannot create the reference image folder.");
        return {};
    }
    QString base = QFileInfo(name).completeBaseName();
    if (base.isEmpty() || base == "." || base == "..")
        base = "reference";
    QString id = base + ".png";
    for (int n = 2; QFileInfo::exists(QDir(m_directory).filePath(id)); ++n)
        id = base + QString("-%1.png").arg(n);
    QImage image = input.width() > 512 || input.height() > 512
        ? input.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : input;
    if (image.colorSpace().isValid())
        image.convertToColorSpace(QColorSpace::SRgb);
    image.setColorSpace(QColorSpace::SRgb);
    QSaveFile file(path(id));
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        *error = i18n("Cannot save the reference image: %1", file.errorString());
        return {};
    }
    return id;
}
bool Library::remove(const QString &id)
{
    const QString file = path(id);
    return !file.isEmpty() && QFile::remove(file);
}
QImage Library::importImage(const QString &fileName, QString *error)
{
    std::unique_ptr<KisDocument> doc(KisPart::instance()->createDocument());
    doc->setFileBatchMode(true);
    KisImportExportManager manager(doc.get());
    const auto status = manager.importDocument(fileName, QString());
    if (!status.isOk() || !doc->image()) {
        *error = status.errorMessage();
        return {};
    }
    doc->image()->refreshGraphAsync();
    doc->image()->waitForDone();
    const auto thumbnail = doc->image()->projection()->createThumbnailDevice(512, 512, doc->image()->bounds());
    QImage image =
        thumbnail->convertToQImage(KoColorSpaceRegistry::instance()->rgb8()->profile(), thumbnail->exactBounds());
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
} // namespace ColorReference
