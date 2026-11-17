/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KIS_COLOR_REFERENCE_CONFIG_WIDGET_H
#define KIS_COLOR_REFERENCE_CONFIG_WIDGET_H
#include "KisColorReferenceResources.h"
#include <filter/kis_filter_configuration.h>
#include <kis_config_widget.h>
class QComboBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class KisDoubleSliderSpinBox;
class KisGradientChooser;
class KisColorReferenceConfigWidget : public KisConfigWidget
{
public:
    KisColorReferenceConfigWidget(QWidget *parent,
                                  KisPaintDeviceSP device,
                                  const QString &libraryDirectory = QString());
    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;
    void setView(KisViewManager *view) override;
    void setCanvasResourcesInterface(KoCanvasResourcesInterfaceSP resources) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshLibrary();
    void selectImage();
    void updateGradient(KoAbstractGradientSP gradient);
    void updateGradientPreview();
    void editGradient();
    void loadImage();
    void importImages(const QStringList &filenames);
    void deleteImage();
    QComboBox *m_source;
    QStackedWidget *m_pages;
    QListWidget *m_images;
    QPushButton *m_delete;
    QLabel *m_notice;
    KisGradientChooser *m_gradient;
    QLabel *m_gradientPreview;
    KoAbstractGradientSP m_currentGradient;
    KisDoubleSliderSpinBox *m_amount;
    QCheckBox *m_keep;
    ColorReference::Library m_library;
    KisFilterConfigurationSP m_stored;
    bool m_importing = false;
};
#endif
