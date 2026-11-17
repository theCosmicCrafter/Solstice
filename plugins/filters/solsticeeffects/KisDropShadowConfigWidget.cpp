/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KisDropShadowConfigWidget.h"
#include <KisGlobalResourcesInterface.h>
#include <KoColorSpaceRegistry.h>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <cmath>
#include <filter/kis_filter_configuration.h>
#include <functional>
#include <kis_color_button.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
class KisShadowOffsetPad : public QWidget
{
public:
    explicit KisShadowOffsetPad(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName("dropShadowOffsetPad");
        setFixedSize(176, 192);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(i18n("Shadow position"));
        setToolTip(
            i18n("Drag to set the shadow direction and distance. Arrow keys move 1 pixel; Shift moves 10 pixels. Right "
                 "is 0 degrees; down is 90 degrees."));
    }
    std::function<void(double, double)> changed;
    void setOffset(double angle, double distance)
    {
        m_angle = angle;
        m_offset = QPointF(std::cos(angle * M_PI / 180), std::sin(angle * M_PI / 180)) * distance;
        if (!m_dragging)
            m_scale = qMax(32.0, std::ceil(distance / 32) * 32);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        p.setPen(palette().mid().color());
        p.drawEllipse(QPointF(88, 80), 70, 70);
        p.drawLine(18, 80, 158, 80);
        p.drawLine(88, 10, 88, 150);
        p.setPen(QPen(palette().highlight().color(), hasFocus() ? 3 : 2));
        const QPointF handle = QPointF(88, 80) + m_offset * (70 / m_scale);
        p.drawLine(QPointF(88, 80), handle);
        p.setBrush(palette().highlight());
        p.drawEllipse(handle, 5, 5);
        p.setPen(palette().text().color());
        p.drawText(QRect(0, 157, width(), 32), Qt::AlignCenter, i18n("Radius: %1 px", int(m_scale)));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        m_dragging = true;
        setFocus(Qt::MouseFocusReason);
        moveHandle(e->localPos());
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging && e->buttons().testFlag(Qt::LeftButton)) {
            moveHandle(e->localPos());
            e->accept();
        } else
            QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_dragging) {
            moveHandle(e->localPos());
            m_dragging = false;
            setOffset(m_angle, std::hypot(m_offset.x(), m_offset.y()));
            e->accept();
        } else
            QWidget::mouseReleaseEvent(e);
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::UngrabMouse || e->type() == QEvent::WindowDeactivate || e->type() == QEvent::Hide)
            m_dragging = false;
        return QWidget::event(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        const double step = e->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
        QPointF delta;
        switch (e->key()) {
        case Qt::Key_Left:
            delta.setX(-step);
            break;
        case Qt::Key_Right:
            delta.setX(step);
            break;
        case Qt::Key_Up:
            delta.setY(-step);
            break;
        case Qt::Key_Down:
            delta.setY(step);
            break;
        default:
            QWidget::keyPressEvent(e);
            return;
        }
        publish(m_offset + delta);
        e->accept();
    }

private:
    void moveHandle(QPointF position)
    {
        publish((position - QPointF(88, 80)) * (m_scale / 70));
    }
    void publish(QPointF offset)
    {
        const double length = std::hypot(offset.x(), offset.y());
        const double distance = qMin(1000.0, length);
        const double angle =
            length > 1e-9 ? std::fmod(std::atan2(offset.y(), offset.x()) * 180 / M_PI + 360, 360) : m_angle;
        if (changed)
            changed(angle, distance);
    }
    QPointF m_offset;
    double m_angle = 45, m_scale = 32;
    bool m_dragging = false;
};
KisDropShadowConfigWidget::KisDropShadowConfigWidget(QWidget *parent, KisPaintDeviceSP device)
    : KisConfigWidget(parent)
    , m_blur(new KisDoubleSliderSpinBox(this))
    , m_angle(new KisDoubleSliderSpinBox(this))
    , m_distance(new KisDoubleSliderSpinBox(this))
    , m_opacity(new KisDoubleSliderSpinBox(this))
    , m_color(new KisColorButton(this))
    , m_pad(new KisShadowOffsetPad(this))
{
    auto *layout = new QFormLayout(this);
    m_blur->setObjectName("dropShadowBlur");
    m_angle->setObjectName("dropShadowAngle");
    m_distance->setObjectName("dropShadowDistance");
    m_opacity->setObjectName("dropShadowOpacity");
    m_color->setObjectName("dropShadowColor");
    m_blur->setRange(0, 100, 1);
    m_angle->setRange(0, 359.9, 1);
    m_distance->setRange(0, 1000, 1);
    m_opacity->setRange(0, 100, 1);
    m_color->setAlphaChannelEnabled(false);
    m_color->setColor(KoColor(Qt::black, device ? device->colorSpace() : KoColorSpaceRegistry::instance()->rgb8()));
    m_blur->setValue(20);
    m_angle->setValue(45);
    m_distance->setValue(20);
    m_opacity->setValue(50);
    layout->addRow(i18n("Blur:"), m_blur);
    layout->addRow(i18n("Angle (degrees):"), m_angle);
    layout->addRow(i18n("Distance (pixels):"), m_distance);
    layout->addRow(i18n("Opacity (%):"), m_opacity);
    layout->addRow(i18n("Shadow color:"), m_color);
    layout->addRow(i18n("Position:"), m_pad);
    auto *notice = new QLabel(i18n("Use a layer with transparency, or a filter mask on it. The shadow is hidden behind "
                                   "opaque paint. Angle 0 points right; 90 points down."),
                              this);
    notice->setWordWrap(true);
    layout->addRow(notice);
    for (auto *slider : {m_blur, m_angle, m_distance, m_opacity})
        connect(slider, &KisDoubleSliderSpinBox::valueChanged, this, [this] {
            if (m_loading)
                return;
            m_pad->setOffset(m_angle->value(), m_distance->value());
            Q_EMIT sigConfigurationItemChanged();
        });
    connect(m_color, &KisColorButton::changed, this, [this] {
        if (!m_loading)
            Q_EMIT sigConfigurationItemChanged();
    });
    m_pad->changed = [this](double angle, double distance) {
        const QSignalBlocker a(m_angle), d(m_distance);
        m_angle->setValue(angle);
        m_distance->setValue(distance);
        m_pad->setOffset(m_angle->value(), m_distance->value());
        Q_EMIT sigConfigurationItemChanged();
    };
    m_pad->setOffset(45, 20);
}
void KisDropShadowConfigWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    m_loading = true;
    m_blur->setValue(config->getDouble("blur", 20));
    m_angle->setValue(config->getDouble("angle", 45));
    m_distance->setValue(config->getDouble("distance", 20));
    m_opacity->setValue(config->getDouble("opacity", 50));
    m_color->setColor(config->getColor("shadowColor", m_color->color()));
    m_pad->setOffset(m_angle->value(), m_distance->value());
    m_loading = false;
}
KisPropertiesConfigurationSP KisDropShadowConfigWidget::configuration() const
{
    KisFilterConfigurationSP config =
        new KisFilterConfiguration("solstice_drop_shadow", 1, KisGlobalResourcesInterface::instance());
    config->setProperty("blur", m_blur->value());
    config->setProperty("angle", m_angle->value());
    config->setProperty("distance", m_distance->value());
    config->setProperty("opacity", m_opacity->value());
    config->setProperty("shadowColor", QVariant::fromValue(m_color->color()));
    return config;
}
