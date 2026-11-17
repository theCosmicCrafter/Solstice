/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickAccessGestureDialog.h"
#include "QuickAccessGesturePresentation.h"

#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceModel.h>
#include <KisResourceServerProvider.h>
#include <KisViewManager.h>
#include <KoResource.h>
#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using namespace QuickAccessGesturePresentation;

QuickAccessGestureDialog::QuickAccessGestureDialog(const QuickAccess::Document &document, QWidget *parent)
    : QDialog(parent)
    , m_document(&document)
{
    setWindowTitle(i18nc("@title:window", "Gesture Configuration"));
    resize(650, 560);
    for (const QJsonValue &value : document.gesturePages)
        m_pages.append(value.toObject());

    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("QuickAccessGesture"));
    auto *root = new QVBoxLayout(this);
    auto *settings = new QFormLayout();
    m_enabled = new QCheckBox(this);
    m_enabled->setChecked(config.readEntry("Enabled", true));
    m_threshold = new QSpinBox(this);
    m_threshold->setRange(1, 200);
    m_threshold->setSuffix(i18nc("@item:valuesuffix", " px"));
    m_threshold->setValue(config.readEntry("MinimumPixelsToMove", 20));
    m_preview = new QCheckBox(this);
    m_preview->setChecked(config.readEntry("ShowPreview", true));
    settings->addRow(i18nc("@label", "Enable gesture system:"), m_enabled);
    settings->addRow(i18nc("@label:spinbox", "Minimum movement:"), m_threshold);
    settings->addRow(i18nc("@label", "Show gesture preview:"), m_preview);
    root->addLayout(settings);

    auto *toolbar = new QHBoxLayout();
    auto *addButton = new QToolButton(this);
    addButton->setText(QStringLiteral("+"));
    addButton->setToolTip(i18nc("@info:tooltip", "Add gesture page"));
    auto *removeButton = new QToolButton(this);
    removeButton->setText(QStringLiteral("−"));
    removeButton->setToolTip(i18nc("@info:tooltip", "Remove current gesture page"));
    toolbar->addStretch();
    toolbar->addWidget(addButton);
    toolbar->addWidget(removeButton);
    root->addLayout(toolbar);
    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs);
    if (m_pages.isEmpty())
        m_pages.append(QJsonObject());
    for (int index = 0; index < m_pages.size(); ++index)
        m_tabs->addTab(createPage(index), QString::number(index + 1));
    connect(addButton, &QToolButton::clicked, this, [this]() {
        addPage();
    });
    connect(removeButton, &QToolButton::clicked, this, [this]() {
        const int index = m_tabs->currentIndex();
        if (index < 0 || m_pages.size() <= 1)
            return;
        m_pages.removeAt(index);
        while (m_tabs->count()) {
            QWidget *page = m_tabs->widget(0);
            m_tabs->removeTab(0);
            page->deleteLater();
        }
        for (int tab = 0; tab < m_pages.size(); ++tab)
            m_tabs->addTab(createPage(tab), QString::number(tab + 1));
        m_tabs->setCurrentIndex(qMin(index, m_tabs->count() - 1));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QJsonArray QuickAccessGestureDialog::pages() const
{
    QJsonArray result;
    for (const QJsonObject &page : m_pages)
        result.append(page);
    return result;
}

bool QuickAccessGestureDialog::gesturesEnabled() const
{
    return m_enabled->isChecked();
}

bool QuickAccessGestureDialog::showPreview() const
{
    return m_preview->isChecked();
}

int QuickAccessGestureDialog::movementThreshold() const
{
    return m_threshold->value();
}

void QuickAccessGestureDialog::addPage(const QJsonObject &page)
{
    const int index = m_pages.size();
    m_pages.append(page);
    m_tabs->addTab(createPage(index), QString::number(index + 1));
    m_tabs->setCurrentIndex(index);
}

QWidget *QuickAccessGestureDialog::createPage(int pageIndex)
{
    auto *page = new QWidget(m_tabs);
    auto *layout = new QVBoxLayout(page);
    auto *key = new QLineEdit(m_pages.at(pageIndex).value(QStringLiteral("gesture_key")).toString(), page);
    key->setMaxLength(12);
    auto *form = new QFormLayout();
    form->addRow(i18nc("@label:textbox", "Gesture key:"), key);
    layout->addLayout(form);
    connect(key, &QLineEdit::textChanged, this, [this, pageIndex](const QString &text) {
        m_pages[pageIndex].insert(QStringLiteral("gesture_key"), text.trimmed().toUpper());
    });

    auto *grid = new QGridLayout();
    grid->setSpacing(10);
    grid->setAlignment(Qt::AlignCenter);
    const QList<QPoint> resourcePositions{QPoint(0, 0),
                                          QPoint(0, 2),
                                          QPoint(0, 4),
                                          QPoint(2, 0),
                                          QPoint(2, 2),
                                          QPoint(2, 4),
                                          QPoint(4, 0),
                                          QPoint(4, 2),
                                          QPoint(4, 4)};
    const QList<QPoint> arrowPositions{QPoint(1, 1),
                                       QPoint(1, 2),
                                       QPoint(1, 3),
                                       QPoint(2, 1),
                                       QPoint(-1, -1),
                                       QPoint(2, 3),
                                       QPoint(3, 1),
                                       QPoint(3, 2),
                                       QPoint(3, 3)};
    for (int index = 0; index < Directions.size(); ++index) {
        const QString direction = Directions.at(index);
        const QJsonObject config = m_pages.at(pageIndex).value(direction).toObject();
        auto *resourceButton = new QPushButton(page);
        resourceButton->setFixedSize(100, 82);
        resourceButton->setToolTip(directionLabel(config));
        updateResourceButton(resourceButton, config);
        connect(resourceButton, &QPushButton::clicked, this, [this, pageIndex, direction, resourceButton]() {
            editDirection(pageIndex, direction, resourceButton);
        });
        const QPoint resourcePosition = resourcePositions.at(index);
        grid->addWidget(resourceButton, resourcePosition.x(), resourcePosition.y(), Qt::AlignCenter);

        const QPoint arrowPosition = arrowPositions.at(index);
        if (arrowPosition.x() >= 0) {
            auto *arrowButton = new QPushButton(page);
            arrowButton->setFixedSize(64, 64);
            arrowButton->setIcon(QIcon(QStringLiteral(":/quickaccess/gesture/%1.png").arg(direction)));
            arrowButton->setIconSize(QSize(48, 48));
            arrowButton->setToolTip(direction);
            connect(arrowButton, &QPushButton::clicked, this, [this, pageIndex, direction, resourceButton]() {
                editDirection(pageIndex, direction, resourceButton);
            });
            grid->addWidget(arrowButton, arrowPosition.x(), arrowPosition.y(), Qt::AlignCenter);
        }
    }
    layout->addLayout(grid);
    layout->addStretch();
    return page;
}

void QuickAccessGestureDialog::editDirection(int pageIndex, const QString &direction, QPushButton *button)
{
    const QJsonObject existing = m_pages.at(pageIndex).value(direction).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Configure %1 Gesture", direction));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *type = new QComboBox(&dialog);
    type->addItem(i18nc("@item:inlistbox", "Empty"), QString());
    type->addItem(i18nc("@item:inlistbox", "Action"), QStringLiteral("action"));
    type->addItem(i18nc("@item:inlistbox", "Brush"), QStringLiteral("brush"));
    type->addItem(i18nc("@item:inlistbox", "Docker"), QStringLiteral("docker_toggle"));
    const int typeIndex = type->findData(existing.value(QStringLiteral("gesture_type")).toString());
    type->setCurrentIndex(qMax(0, typeIndex));
    auto *value = new QComboBox(&dialog);
    value->setEditable(true);
    value->setInsertPolicy(QComboBox::NoInsert);
    form->addRow(i18nc("@label:listbox", "Type:"), type);
    form->addRow(i18nc("@label:listbox", "Resource:"), value);
    layout->addLayout(form);
    const auto populate = [type, value, existing]() {
        const QString selectedType = type->currentData().toString();
        const QString previous =
            value->currentData().toString().isEmpty() ? configuredValue(existing) : value->currentData().toString();
        value->clear();
        QSet<QString> identifiers;
        if (selectedType == QStringLiteral("action")) {
            if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
                QList<QAction *> actions = window->viewManager()->actionCollection()->actions();
                std::sort(actions.begin(), actions.end(), [](QAction *a, QAction *b) {
                    return a->text().localeAwareCompare(b->text()) < 0;
                });
                for (QAction *action : std::as_const(actions)) {
                    const QString id = action->objectName();
                    if (id.isEmpty() || identifiers.contains(id))
                        continue;
                    identifiers.insert(id);
                    QString label = action->text();
                    label.remove(QLatin1Char('&'));
                    value->addItem(label.isEmpty() ? id : label, id);
                }
            }
        } else if (selectedType == QStringLiteral("brush")) {
            auto *model = KisResourceServerProvider::instance()->paintOpPresetServer()->resourceModel();
            for (int row = 0; row < model->rowCount(); ++row) {
                const KoResourceSP resource =
                    model->resourceForIndex(model->index(row, KisAbstractResourceModel::Name));
                if (!resource || identifiers.contains(resource->name()))
                    continue;
                identifiers.insert(resource->name());
                value->addItem(QIcon(QPixmap::fromImage(resource->image())), resource->name(), resource->name());
            }
            value->model()->sort(0);
        } else if (selectedType == QStringLiteral("docker_toggle")) {
            if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
                QList<QDockWidget *> dockers = window->findChildren<QDockWidget *>();
                std::sort(dockers.begin(), dockers.end(), [](QDockWidget *a, QDockWidget *b) {
                    return a->windowTitle().localeAwareCompare(b->windowTitle()) < 0;
                });
                for (QDockWidget *docker : std::as_const(dockers)) {
                    const QString id = docker->objectName();
                    if (id.isEmpty() || identifiers.contains(id))
                        continue;
                    identifiers.insert(id);
                    value->addItem(docker->windowTitle().isEmpty() ? id : docker->windowTitle(), id);
                }
            }
        }
        const int index = value->findData(previous);
        if (index >= 0)
            value->setCurrentIndex(index);
        else
            value->setEditText(previous);
    };
    connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, populate);
    populate();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString selectedType = type->currentData().toString();
    const QString selectedValue =
        value->currentData().toString().isEmpty() ? value->currentText().trimmed() : value->currentData().toString();
    if (selectedType.isEmpty() || selectedValue.isEmpty()) {
        m_pages[pageIndex].remove(direction);
    } else {
        QString parameterName = QStringLiteral("action_id");
        if (selectedType == QStringLiteral("brush"))
            parameterName = QStringLiteral("brush_name");
        else if (selectedType == QStringLiteral("docker_toggle"))
            parameterName = QStringLiteral("docker_name");
        m_pages[pageIndex].insert(
            direction,
            QJsonObject{{QStringLiteral("gesture_type"), selectedType},
                        {QStringLiteral("parameters"), QJsonObject{{parameterName, selectedValue}}}});
    }
    updateResourceButton(button, m_pages.at(pageIndex).value(direction).toObject());
}

void QuickAccessGestureDialog::updateResourceButton(QPushButton *button, const QJsonObject &config) const
{
    const QIcon icon = gestureIcon(*m_document, config);
    button->setIcon(icon);
    if (icon.isNull()) {
        button->setText(directionLabel(config));
    } else {
        button->setText(QString());
        button->setIconSize(QSize(64, 64));
    }
    button->setToolTip(directionLabel(config));
}

QString QuickAccessGestureDialog::directionLabel(const QJsonObject &config) const
{
    if (config.isEmpty())
        return i18nc("@label", "[Empty]");
    const QString type = config.value(QStringLiteral("gesture_type")).toString();
    const QString value = configuredValue(config);
    if (type == QStringLiteral("action"))
        return i18n("Action:\n%1", value);
    if (type == QStringLiteral("brush"))
        return i18n("Brush:\n%1", value);
    if (type == QStringLiteral("docker_toggle"))
        return i18n("Docker:\n%1", value);
    return value;
}
