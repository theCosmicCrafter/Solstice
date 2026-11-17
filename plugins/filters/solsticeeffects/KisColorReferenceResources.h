/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_COLOR_REFERENCE_RESOURCES_H
#define KIS_COLOR_REFERENCE_RESOURCES_H
#include <KoAbstractGradient.h>
#include <QImage>
#include <QStringList>
#include <array>

class KoColorSpace;
namespace ColorReference
{
using RGB = std::array<double, 3>;
using Table = QVector<RGB>;
constexpr int TableSize = 1024;
const KoColorSpace *workingSpace();
double brightness(const RGB &color);
RGB lookup(const Table &table, double position);
RGB keepBrightness(RGB color, double brightness);
Table fromImage(const QImage &image);
Table fromGradient(KoAbstractGradientSP gradient);
QString encode(const Table &table);
Table decode(const QString &encoded);
QString gradientXml(KoAbstractGradientSP gradient);
KoAbstractGradientSP gradientFromXml(const QString &xml);
KoAbstractGradientSP defaultGradient();
QStringList presetNames();
QImage presetImage(int index);

// Stores bounded, color-managed sRGB previews, not the user's source files.
class Library
{
public:
    explicit Library(QString directory);
    QStringList entries() const;
    QImage image(const QString &id) const;
    QString add(const QImage &image, const QString &name, QString *error);
    bool remove(const QString &id);
    static QImage importImage(const QString &fileName, QString *error);

private:
    QString path(const QString &id) const;
    QString m_directory;
};
} // namespace ColorReference
#endif
