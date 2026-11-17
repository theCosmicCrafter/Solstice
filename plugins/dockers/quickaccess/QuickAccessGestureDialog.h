/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef QUICKACCESSGESTUREDIALOG_H
#define QUICKACCESSGESTUREDIALOG_H
#include "QuickAccessModel.h"
#include <QDialog>
class QCheckBox;
class QPushButton;
class QSpinBox;
class QTabWidget;

class QuickAccessGestureDialog : public QDialog
{
public:
    explicit QuickAccessGestureDialog(const QuickAccess::Document &document, QWidget *parent = nullptr);

    QJsonArray pages() const;
    bool gesturesEnabled() const;
    bool showPreview() const;
    int movementThreshold() const;

private:
    void addPage(const QJsonObject &page = QJsonObject());
    QWidget *createPage(int pageIndex);
    void editDirection(int pageIndex, const QString &direction, QPushButton *button);
    void updateResourceButton(QPushButton *button, const QJsonObject &config) const;
    QString directionLabel(const QJsonObject &config) const;

    const QuickAccess::Document *m_document{nullptr};
    QList<QJsonObject> m_pages;
    QTabWidget *m_tabs{nullptr};
    QCheckBox *m_enabled{nullptr};
    QCheckBox *m_preview{nullptr};
    QSpinBox *m_threshold{nullptr};
};

#endif
