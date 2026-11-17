/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisLazyToolsRenamePresets.h"

#include <KoResourcePaths.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>

int KisLazyToolsRenamePresets::colorLabelIndex(const QString &name)
{
    static const QHash<QString, int> labels = {
        {QStringLiteral("blue"), 1},
        {QStringLiteral("green"), 2},
        {QStringLiteral("yellow"), 3},
        {QStringLiteral("orange"), 4},
        {QStringLiteral("brown"), 5},
        {QStringLiteral("red"), 6},
        {QStringLiteral("purple"), 7},
        {QStringLiteral("grey"), 8},
        {QStringLiteral("gray"), 8},
    };
    return labels.value(name.trimmed().toLower(), 0);
}

QString KisLazyToolsRenamePresets::colorLabelName(int index)
{
    static const QStringList names = {QString(),
                                      QStringLiteral("Blue"),
                                      QStringLiteral("Green"),
                                      QStringLiteral("Yellow"),
                                      QStringLiteral("Orange"),
                                      QStringLiteral("Brown"),
                                      QStringLiteral("Red"),
                                      QStringLiteral("Purple"),
                                      QStringLiteral("Grey")};
    return names.value(index);
}

QString KisLazyToolsRenamePresets::filePath()
{
    return QDir(KoResourcePaths::getAppDataLocation())
        .filePath(QStringLiteral("lazy_tools/config/name_color_list.txt"));
}

QList<KisLazyToolsRenamePresets::Preset> KisLazyToolsRenamePresets::load(const QString &path, bool *opened)
{
    QList<Preset> result;
    QFile file(path);
    const bool readable = file.open(QIODevice::ReadOnly | QIODevice::Text);
    if (opened)
        *opened = readable;
    if (readable) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const qsizetype comma = line.indexOf(QLatin1Char(','));
            const QString name = (comma < 0 ? line : line.left(comma)).trimmed();
            const int label = comma < 0 ? 0 : colorLabelIndex(line.mid(comma + 1));
            if (!name.isEmpty())
                result.append({name, label});
        }
    }
    return result;
}

QString KisLazyToolsRenamePresets::readText(const QString &path, bool *opened)
{
    QFile file(path);
    const bool readable = file.open(QIODevice::ReadOnly | QIODevice::Text);
    if (opened)
        *opened = readable;
    return readable ? QTextStream(&file).readAll() : QString();
}

bool KisLazyToolsRenamePresets::writeText(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream stream(&file);
    stream << text;
    stream.flush();
    return stream.status() == QTextStream::Ok;
}

void KisLazyToolsRenamePresets::append(const QString &path, const QString &name, int label)
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QStringList lines;
    QFile input(path);
    if (input.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&input);
        while (!stream.atEnd()) {
            lines << stream.readLine();
        }
    }
    const QString newLine = label > 0 ? QStringLiteral("%1, %2").arg(name, colorLabelName(label)) : name;
    if (!lines.contains(newLine)) {
        lines << newLine;
        QFile output(path);
        if (output.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&output);
            stream << lines.join(QLatin1Char('\n'));
        }
    }
}
