/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestControlSupport.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <QWidget>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <kis_canvas2.h>
#include <kis_image.h>
#include <kis_paint_device.h>

namespace
{
constexpr qint64 MaxHashedBytes = qint64(512) * 1024 * 1024;
}

namespace TestControlSupport
{
KisMainWindow *currentWindow()
{
    return KisPart::instance()->currentMainwindow();
}

KisViewManager *currentViewManager()
{
    KisMainWindow *window = currentWindow();
    return window ? window->viewManager() : nullptr;
}

QJsonObject rectToJson(const QRect &rect)
{
    return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool readDevice(KisPaintDeviceSP device, const QRect &rect, QByteArray *bytes)
{
    const qint64 size = qint64(rect.width()) * rect.height() * device->pixelSize();
    if (size > MaxHashedBytes) {
        return false;
    }
    bytes->resize(int(size));
    device->readBytes(reinterpret_cast<quint8 *>(bytes->data()), rect);
    return true;
}

QJsonValue documentInfo(KisViewManager *viewManager)
{
    KisImageSP image;
    if (viewManager) {
        image = viewManager->image();
    }
    if (!image) {
        return QJsonValue::Null;
    }
    const KoColorSpace *colorSpace = image->colorSpace();
    KisDocument *document = viewManager->document();
    return QJsonObject{
        {"width", image->width()},
        {"height", image->height()},
        {"colorModel", colorSpace->colorModelId().id()},
        {"colorDepth", colorSpace->colorDepthId().id()},
        {"profile", colorSpace->profile() ? colorSpace->profile()->name() : QString()},
        // The file name only, so results do not depend on where the test root is.
        {"fileName", document ? QFileInfo(document->path()).fileName() : QString()},
        {"modified", document && document->isModified()},
    };
}

QJsonValue canvasInfo(KisViewManager *viewManager)
{
    KisCanvas2 *canvas = viewManager ? viewManager->canvasBase() : nullptr;
    QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
    if (!widget) {
        return QJsonValue::Null;
    }
    return QJsonObject{
        {"widgetClass", QString::fromLatin1(widget->metaObject()->className())},
        {"width", widget->width()},
        {"height", widget->height()},
        {"hasFocus", widget->hasFocus()},
        {"windowActive", widget->isActiveWindow()},
    };
}

QJsonObject objectSchema(const QJsonObject &properties, const QStringList &required)
{
    QJsonObject schema{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
    if (!required.isEmpty()) {
        schema.insert("required", QJsonArray::fromStringList(required));
    }
    return schema;
}

QJsonObject schemaProperty(const QString &type, const QString &description)
{
    return {{"type", type}, {"description", description}};
}

QJsonObject enumProperty(const QStringList &values, const QString &description)
{
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}, {"description", description}};
}

void pollUntil(QObject *context,
               std::function<bool()> condition,
               int timeoutMs,
               std::function<void(bool, qint64)> done,
               int stablePolls)
{
    QElapsedTimer clock;
    clock.start();
    int satisfiedPolls = condition() ? 1 : 0;
    if (satisfiedPolls >= stablePolls) {
        done(true, 0);
        return;
    }

    QTimer *timer = new QTimer(context);
    timer->setInterval(20);
    QObject::connect(timer,
                     &QTimer::timeout,
                     context,
                     [timer, clock, condition, timeoutMs, done, stablePolls, satisfiedPolls]() mutable {
                         satisfiedPolls = condition() ? satisfiedPolls + 1 : 0;
                         const bool satisfied = satisfiedPolls >= stablePolls;
                         if (!satisfied && clock.elapsed() < timeoutMs) {
                             return;
                         }
                         timer->stop();
                         timer->deleteLater();
                         done(satisfied, clock.elapsed());
                     });
    timer->start();
}
} // namespace TestControlSupport
