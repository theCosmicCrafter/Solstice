#include "ESRGANFilter.h"

#include "KisGlobalResourcesInterface.h"
#include "KoUpdater.h"
#include "kis_config_widget.h"
#include "kis_filter_category_ids.h"
#include "kis_paint_device.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

class ESRGANWidget : public KisConfigWidget
{
    Q_OBJECT
public:
    ESRGANWidget(QSharedPointer<VisionModels> vision, QWidget *parent)
        : KisConfigWidget(parent)
        , m_vision(vision)
    {
        QVBoxLayout *layout = new QVBoxLayout(this);

        QLabel *label = new QLabel(i18n("Uses RealESRGAN neural network for detail restoration and super-resolution."), this);
        layout->addWidget(label);

        m_modelSelectWidget = new VisionMLModelSelect(m_vision, VisionMLTask::restoration, true);
        layout->addWidget(m_modelSelectWidget);

        QLabel *modelHint = new QLabel(
            i18n("Restores fine details and sharp edges. Can upscale or restore to current layer bounds."),
            this);
        modelHint->setWordWrap(true);
        layout->addWidget(modelHint);

        VisionMLBackendWidget *backendSelect = new VisionMLBackendWidget(m_vision, true);
        layout->addWidget(backendSelect);

        m_fitToOriginalCheckBox = new QCheckBox(i18n("Restore to current bounds (1x Detail Restoration)"), this);
        m_fitToOriginalCheckBox->setChecked(true);
        layout->addWidget(m_fitToOriginalCheckBox);

        layout->addStretch();

        connect(m_vision.get(), &VisionModels::modelNameChanged, this, &ESRGANWidget::handleModelChange);
        connect(m_vision.get(), &VisionModels::backendChanged, this, &ESRGANWidget::handleBackendChange);
        connect(m_fitToOriginalCheckBox,
                &QCheckBox::stateChanged,
                this,
                &KisConfigWidget::sigConfigurationItemChanged);
    }

    void setConfiguration(const KisPropertiesConfigurationSP config) override
    {
        QVariant value;
        if (config->getProperty("model", value)) {
            m_vision->setModelName(VisionMLTask::restoration, value.toString());
        }
        if (config->getProperty("backend", value)) {
            auto backend = value.toString() == "gpu" ? visp::backend_type::gpu : visp::backend_type::cpu;
            m_vision->setBackend(backend);
        }
        if (config->getProperty("fit_to_original", value)) {
            m_fitToOriginalCheckBox->setChecked(value.toBool());
        }
    }

    KisPropertiesConfigurationSP configuration() const override
    {
        KisFilterConfigurationSP config =
            new KisFilterConfiguration("esrgan_restoration", 1, KisGlobalResourcesInterface::instance());
        config->setProperty("model", m_vision->modelName(VisionMLTask::restoration));
        config->setProperty("backend", m_vision->backend() == visp::backend_type::gpu ? "gpu" : "cpu");
        config->setProperty("fit_to_original", m_fitToOriginalCheckBox->isChecked());
        return config;
    }

    void handleModelChange(VisionMLTask task, QString const &)
    {
        if (task == VisionMLTask::restoration) {
            emit sigConfigurationItemChanged();
        }
    }

    void handleBackendChange(visp::backend_type)
    {
        emit sigConfigurationItemChanged();
    }

private:
    QSharedPointer<VisionModels> m_vision;
    VisionMLModelSelect *m_modelSelectWidget = nullptr;
    QCheckBox *m_fitToOriginalCheckBox = nullptr;
};

//
// ESRGANFilter implementation

ESRGANFilter::ESRGANFilter(QSharedPointer<VisionModels> vision)
    : KisFilter(id(), FiltersCategoryOtherId, i18n("ESRGAN Super-Resolution..."))
    , m_vision(vision)
{
    setSupportsPainting(false);
    setSupportsAdjustmentLayers(false);
    setSupportsThreading(false);
    setSupportsLevelOfDetail(false);
    setColorSpaceIndependence(TO_RGBA8);
}

KisConfigWidget *ESRGANFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP, bool) const
{
    return new ESRGANWidget(m_vision, parent);
}

KisFilterConfigurationSP ESRGANFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    config->setProperty("model", m_vision->modelName(VisionMLTask::restoration));
    config->setProperty("backend", m_vision->backend() == visp::backend_type::gpu ? "gpu" : "cpu");
    config->setProperty("fit_to_original", true);
    return config;
}

void ESRGANFilter::processImpl(KisPaintDeviceSP device,
                               const QRect &applyRect,
                               const KisFilterConfigurationSP config,
                               KoUpdater *progressUpdater) const
{
    if (progressUpdater) {
        progressUpdater->setAutoNestedName(i18n("ESRGAN Super-Resolution"));
    }

    VisionMLImage image = VisionMLImage::prepare(*device, applyRect);
    if (!image) {
        qWarning() << "ESRGAN: No image data available in the specified rectangle.";
        return;
    }
    if (image.view.extent[0] < 16 || image.view.extent[1] < 16) {
        qWarning() << "ESRGAN: Image is too small, minimum size is 16x16 pixels.";
        return;
    }

    if (progressUpdater)
        progressUpdater->setProgress(10);

    bool fitToOriginal = true;
    if (QVariant configValue; config->getProperty("fit_to_original", configValue)) {
        fitToOriginal = configValue.toBool();
    }

    try {
        visp::image_data upscaled = m_vision->restore(image.view);
        if (progressUpdater)
            progressUpdater->setProgress(85);

        QImage qimg = VisionMLImage::convertToQImage(upscaled);

        if (fitToOriginal) {
            QImage downscaled = qimg.scaled(applyRect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            device->convertFromQImage(downscaled, nullptr, applyRect.x(), applyRect.y());
        } else {
            device->convertFromQImage(qimg, nullptr, applyRect.x(), applyRect.y());
        }

        if (progressUpdater)
            progressUpdater->setProgress(100);
    } catch (const std::exception &e) {
        Q_EMIT m_report.errorOccurred(QString(e.what()));
    }
}

QRect ESRGANFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP, int) const
{
    return rect;
}

QRect ESRGANFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    bool fitToOriginal = true;
    if (config && config->hasProperty("fit_to_original")) {
        fitToOriginal = config->getBool("fit_to_original");
    }
    if (fitToOriginal) {
        return rect;
    }
    return QRect(rect.x(), rect.y(), rect.width() * 4, rect.height() * 4);
}

#include "ESRGANFilter.moc"
