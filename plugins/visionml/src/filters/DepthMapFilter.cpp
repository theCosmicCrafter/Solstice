#include "DepthMapFilter.h"

#include "KisGlobalResourcesInterface.h"
#include "KoUpdater.h"
#include "kis_config_widget.h"
#include "kis_filter_category_ids.h"
#include "kis_paint_device.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

class DepthMapWidget : public KisConfigWidget
{
    Q_OBJECT
public:
    DepthMapWidget(QSharedPointer<VisionModels> vision, QWidget *parent)
        : KisConfigWidget(parent)
        , m_vision(vision)
    {
        QVBoxLayout *layout = new QVBoxLayout(this);

        QLabel *label = new QLabel(i18n("Uses Depth Anything V2 neural network to estimate monocular depth."), this);
        layout->addWidget(label);

        m_modelSelectWidget = new VisionMLModelSelect(m_vision, VisionMLTask::depth_estimation, true);
        layout->addWidget(m_modelSelectWidget);

        QLabel *modelHint = new QLabel(
            i18n("Generates a grayscale depth map representing relative distance from camera."),
            this);
        modelHint->setWordWrap(true);
        layout->addWidget(modelHint);

        VisionMLBackendWidget *backendSelect = new VisionMLBackendWidget(m_vision, true);
        layout->addWidget(backendSelect);

        m_invertCheckBox = new QCheckBox(i18n("Invert depth map (white = near)"), this);
        layout->addWidget(m_invertCheckBox);

        layout->addStretch();

        connect(m_vision.get(), &VisionModels::modelNameChanged, this, &DepthMapWidget::handleModelChange);
        connect(m_vision.get(), &VisionModels::backendChanged, this, &DepthMapWidget::handleBackendChange);
        connect(m_invertCheckBox,
                &QCheckBox::stateChanged,
                this,
                &KisConfigWidget::sigConfigurationItemChanged);
    }

    void setConfiguration(const KisPropertiesConfigurationSP config) override
    {
        QVariant value;
        if (config->getProperty("model", value)) {
            m_vision->setModelName(VisionMLTask::depth_estimation, value.toString());
        }
        if (config->getProperty("backend", value)) {
            auto backend = value.toString() == "gpu" ? visp::backend_type::gpu : visp::backend_type::cpu;
            m_vision->setBackend(backend);
        }
        if (config->getProperty("invert", value)) {
            m_invertCheckBox->setChecked(value.toBool());
        }
    }

    KisPropertiesConfigurationSP configuration() const override
    {
        KisFilterConfigurationSP config =
            new KisFilterConfiguration("depth_map", 1, KisGlobalResourcesInterface::instance());
        config->setProperty("model", m_vision->modelName(VisionMLTask::depth_estimation));
        config->setProperty("backend", m_vision->backend() == visp::backend_type::gpu ? "gpu" : "cpu");
        config->setProperty("invert", m_invertCheckBox->isChecked());
        return config;
    }

    void handleModelChange(VisionMLTask task, QString const &)
    {
        if (task == VisionMLTask::depth_estimation) {
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
    QCheckBox *m_invertCheckBox = nullptr;
};

//
// DepthMapFilter implementation

DepthMapFilter::DepthMapFilter(QSharedPointer<VisionModels> vision)
    : KisFilter(id(), FiltersCategoryOtherId, i18n("Depth Map..."))
    , m_vision(vision)
{
    setSupportsPainting(false);
    setSupportsAdjustmentLayers(false);
    setSupportsThreading(false);
    setSupportsLevelOfDetail(false);
    setColorSpaceIndependence(TO_RGBA8);
}

KisConfigWidget *DepthMapFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP, bool) const
{
    return new DepthMapWidget(m_vision, parent);
}

KisFilterConfigurationSP DepthMapFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    config->setProperty("model", m_vision->modelName(VisionMLTask::depth_estimation));
    config->setProperty("backend", m_vision->backend() == visp::backend_type::gpu ? "gpu" : "cpu");
    config->setProperty("invert", false);
    return config;
}

void DepthMapFilter::processImpl(KisPaintDeviceSP device,
                                 const QRect &applyRect,
                                 const KisFilterConfigurationSP config,
                                 KoUpdater *progressUpdater) const
{
    if (progressUpdater) {
        progressUpdater->setAutoNestedName(i18n("Depth Map"));
    }

    VisionMLImage image = VisionMLImage::prepare(*device, applyRect);
    if (!image) {
        qWarning() << "Depth Map: No image data available in the specified rectangle.";
        return;
    }
    if (image.view.extent[0] < 64 || image.view.extent[1] < 64) {
        qWarning() << "Depth Map: Image is too small, minimum size is 64x64 pixels.";
        return;
    }

    if (progressUpdater)
        progressUpdater->setProgress(10);

    bool invert = false;
    if (QVariant configValue; config->getProperty("invert", configValue)) {
        invert = configValue.toBool();
    }

    if (QVariant configValue; config->getProperty("backend", configValue)) {
        const auto backend =
            configValue.toString() == "gpu" ? visp::backend_type::gpu : visp::backend_type::cpu;
        if (backend != m_vision->backend()) {
            m_vision->setBackend(backend);
        }
    }
    if (QVariant configValue; config->getProperty("model", configValue)) {
        const QString model = configValue.toString();
        if (!model.isEmpty() && model != m_vision->modelName(VisionMLTask::depth_estimation)) {
            m_vision->setModelName(VisionMLTask::depth_estimation, model);
        }
    }

    try {
        visp::image_data depth = m_vision->estimateDepth(image.view);
        if (progressUpdater)
            progressUpdater->setProgress(80);

        int w = depth.extent[0];
        int h = depth.extent[1];
        QImage depthImg(w, h, QImage::Format_ARGB32);
        uint8_t const *src = (uint8_t const *)depth.data.get();
        for (int y = 0; y < h; ++y) {
            QRgb *scanLine = (QRgb *)depthImg.scanLine(y);
            for (int x = 0; x < w; ++x) {
                uint8_t val = src[y * w + x];
                if (invert) {
                    val = 255 - val;
                }
                scanLine[x] = qRgba(val, val, val, 255);
            }
        }

        device->convertFromQImage(depthImg, nullptr, applyRect.x(), applyRect.y());

        if (progressUpdater)
            progressUpdater->setProgress(100);
    } catch (const std::exception &e) {
        Q_EMIT m_report.errorOccurred(QString(e.what()));
    }
}

QRect DepthMapFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP, int) const
{
    return rect;
}

QRect DepthMapFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP, int) const
{
    return rect;
}

#include "DepthMapFilter.moc"
