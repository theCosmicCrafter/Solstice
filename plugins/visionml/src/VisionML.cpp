#include "VisionML.h"

#include "KisOptionButtonStrip.h"
#include "KisRemoteFileFetcher.h"
#include "KoColorProfile.h"
#include "KoColorSpace.h"
#include "KoColorSpaceRegistry.h"
#include "KoJsonTrader.h"
#include "KoResourcePaths.h"
#include "kis_icon_utils.h"
#include "kis_paint_device.h"
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QToolButton>
#include <QUrl>

#include <string>

#include <ggml-backend.h>

namespace
{

void handleGGMLFatalError(const char *error_message)
{
    qCritical() << "[GGML] Fatal error:" << error_message;
    throw visp::exception(error_message);
}

struct Paths {
    QString lib;
    QString userModels;
    QString installedModels;
    QString legacyModels;
} paths;

void initPaths()
{
    const QDir applicationRoot(KoResourcePaths::getApplicationRoot());
    const QDir userRoot(KoResourcePaths::getAppDataLocation());
    paths.lib = applicationRoot.filePath(QStringLiteral("bin/"));
    paths.installedModels = applicationRoot.filePath(QStringLiteral("share/krita/visionml/models/"));
    paths.userModels = userRoot.filePath(QStringLiteral("visionml/models/"));
    paths.legacyModels = userRoot.filePath(QStringLiteral("pykrita/vision_tools/models/"));

    for (const QString &folder : {QStringLiteral("sam"), QStringLiteral("birefnet"), QStringLiteral("migan")}) {
        QDir().mkpath(QDir(paths.userModels).filePath(folder));
    }
}

void loadGGMLBackend(char const *name)
{
#if defined(WIN32)
    const QString filename = QStringLiteral("ggml-%1.dll").arg(QString::fromLatin1(name));
#elif defined(__APPLE__)
    const QString filename = QStringLiteral("libggml-%1.dylib").arg(QString::fromLatin1(name));
#else
    const QString filename = QStringLiteral("libggml-%1.so").arg(QString::fromLatin1(name));
#endif
    const QString path = QDir(paths.lib).filePath(filename);
    if (QFileInfo::exists(path)) {
        ggml_backend_load(path.toUtf8().constData());
    }
}

QString modelFolder(VisionMLTask task)
{
    switch (task) {
    case VisionMLTask::segmentation:
        return QStringLiteral("sam");
    case VisionMLTask::background_removal:
        return QStringLiteral("birefnet");
    case VisionMLTask::inpainting:
        return QStringLiteral("migan");
    default:
        return QString();
    }
}

QString findModelPath(VisionMLTask task)
{
    return QDir(paths.userModels).filePath(modelFolder(task));
}

QStringList modelRoots()
{
    return {paths.userModels, paths.installedModels, paths.legacyModels};
}

QString resolveModelPath(const QString &relativePath)
{
    for (const QString &root : modelRoots()) {
        const QString candidate = QDir(root).filePath(relativePath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

char const *toString(VisionMLTask task)
{
    switch (task) {
    case VisionMLTask::segmentation:
        return "segmentation";
    case VisionMLTask::inpainting:
        return "inpainting";
    case VisionMLTask::background_removal:
        return "background_removal";
    default:
        return "unknown";
    }
}

void unloadFromGPU(visp::compute_graph &graph, visp::backend_type devType)
{
    if (devType == visp::backend_type::gpu) {
        graph = {};
    }
}

} // namespace

QSharedPointer<VisionModels> VisionModels::create()
{
    initPaths();

    QSharedPointer<VisionModels> result(new VisionModels());
    if (!result->m_backend) {
        return nullptr;
    }
    return result;
}

VisionModels::VisionModels()
{
    ggml_set_abort_callback(handleGGMLFatalError);

    loadGGMLBackend("cpu");
    loadGGMLBackend("vulkan");

    m_config = KSharedConfig::openConfig()->group("VisionML");
    QString backendString = m_config.readEntry("backend", "cpu");
    visp::backend_type backendType = backendString == "gpu" ? visp::backend_type::gpu : visp::backend_type::cpu;

    configureModel(VisionMLTask::segmentation, "sam/MobileSAM-F16.gguf");
    configureModel(VisionMLTask::inpainting, "migan/MIGAN-512-places2-F16.gguf");
    const QString dynamicBiRefNet = QStringLiteral("birefnet/BiRefNet-dynamic-F16.gguf");
    const QString defaultBiRefNet = resolveModelPath(dynamicBiRefNet).isEmpty()
        ? QStringLiteral("birefnet/BiRefNet-lite-F16.gguf")
        : dynamicBiRefNet;
    configureModel(VisionMLTask::background_removal, defaultBiRefNet);

    QString err = initialize(backendType);
    if (!err.isEmpty()) {
        backendType = backendType == visp::backend_type::gpu ? visp::backend_type::cpu : visp::backend_type::gpu;
        err = initialize(backendType);
    }
    if (!err.isEmpty()) {
        QMessageBox::warning(nullptr,
                             i18nc("@title:window", "Solstice - Vision ML"),
                             i18n("Failed to initialize Vision ML.\n") + err);
        return;
    }

    connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()), this, SLOT(cleanUp()));
}

void VisionModels::configureModel(VisionMLTask task, QString const &defaultName)
{
    QString modelName = m_config.readEntry(QString("model_%1").arg(toString(task)), defaultName);
    if (resolveModelPath(modelName).isEmpty()) {
        const QString legacyName = m_config.readEntry(QString("model_%1").arg((int)task), QString());
        if (!legacyName.isEmpty() && !resolveModelPath(legacyName).isEmpty()) {
            modelName = legacyName;
        }
    }
    if (resolveModelPath(modelName).isEmpty()) {
        modelName = defaultName;
    }
    m_modelName[(int)task] = modelName;
}

QString VisionModels::initialize(visp::backend_type backendType)
{
    QMutexLocker lock(&m_mutex);
    m_sam = {};
    m_birefnet = {};
    m_migan = {};
    try {
        m_backend = visp::backend_init(backendType);
    } catch (const std::exception &e) {
        return QString(e.what());
    }
    m_backendType = backendType;

    m_config.writeEntry("backend", backendType == visp::backend_type::gpu ? "gpu" : "cpu");
    return QString();
}

void VisionModels::encodeSegmentationImage(visp::image_view const &image)
{
    QMutexLocker lock(&m_mutex);
    if (!m_sam.weights) {
        unloadModels();
        QByteArray path = modelPath(VisionMLTask::segmentation);
        m_sam = visp::sam_load_model(path.data(), m_backend);
    }
    visp::sam_encode(m_sam, image);
}

bool VisionModels::hasSegmentationImage() const
{
    return m_sam.input_image != nullptr;
}

visp::image_data VisionModels::predictSegmentationMask(visp::i32x2 point)
{
    QMutexLocker lock(&m_mutex);
    return visp::sam_compute(m_sam, point);
}

visp::image_data VisionModels::predictSegmentationMask(visp::box_2d box)
{
    QMutexLocker lock(&m_mutex);
    return visp::sam_compute(m_sam, box);
}

visp::image_data VisionModels::removeBackground(visp::image_view const &originalImage)
{
    QMutexLocker lock(&m_mutex);
    if (!m_birefnet.weights) {
        QByteArray path = modelPath(VisionMLTask::background_removal);
        m_birefnet = visp::birefnet_load_model(path.data(), m_backend);
    }
    visp::image_data resized;
    visp::image_view image = originalImage;
    if (m_birefnet.params.image_size == -1) {
        int maxSide = std::max(image.extent[0], image.extent[1]);
        // BiRefNet-dynamic is trained on images up to 2304px resolution
        // using larger images doesn't make much sense and takes very long.
        // CPU-only because GPU already has built-in resize to stay below allocation limits.
        if (maxSide > 2304 && m_backendType == visp::backend_type::cpu) {
            float f = 2304.f / float(maxSide);
            auto target = visp::i32x2{int(image.extent[0] * f), int(image.extent[1] * f)};
            int m = m_birefnet.params.image_multiple;
            target[0] = (target[0] + m - 1) / m * m;
            target[1] = (target[1] + m - 1) / m * m;
            resized = visp::image_scale(image, target);
            image = resized;
        }
    }
    auto result = visp::birefnet_compute(m_birefnet, image);
    unloadFromGPU(m_birefnet.graph, m_backendType);

    if (resized.data) {
        result = visp::image_scale(result, originalImage.extent);
    }
    return result;
}

visp::image_data VisionModels::inpaint(visp::image_view const &image, visp::image_view const &mask)
{
    QMutexLocker lock(&m_mutex);
    if (!m_migan.weights) {
        QByteArray path = modelPath(VisionMLTask::inpainting);
        m_migan = visp::migan_load_model(path.data(), m_backend);
    }
    return visp::migan_compute(m_migan, image, mask);
}

void VisionModels::unload(VisionMLTask task)
{
    // Models and working memory are not that big.
    // Keep them in RAM for CPU inference to avoid loading models again from disk.
    // Unload from GPU memory because VRAM is more precious.
    if (m_backendType == visp::backend_type::gpu) {
        QMutexLocker lock(&m_mutex);
        switch (task) {
        case VisionMLTask::segmentation:
            m_sam = {};
            break;
        case VisionMLTask::inpainting:
            m_migan = {};
            break;
        case VisionMLTask::background_removal:
            m_birefnet = {};
            break;
        default:
            break;
        }
    }
}

QByteArray VisionModels::modelPath(VisionMLTask task) const
{
    const QString path = resolveModelPath(modelName(task));
    if (path.isEmpty()) {
        throw std::runtime_error("Model file not found: " + modelName(task).toStdString());
    }
    return path.toUtf8();
}

visp::backend_type VisionModels::backend() const
{
    return m_backendType;
}

bool VisionModels::setBackend(visp::backend_type backendType)
{
    if (backendType == m_backendType) {
        return true;
    }
    QString err = initialize(backendType);
    if (!err.isEmpty()) {
        QMessageBox::warning(nullptr,
                             i18nc("@title:window", "Solstice - Vision ML"),
                             i18n("Error while trying to switch inference backend.\n") + err);
        return false;
    }
    Q_EMIT backendChanged(m_backendType);
    return true;
}

QString const &VisionModels::modelName(VisionMLTask task) const
{
    return m_modelName[(int)task];
}

void VisionModels::setModelName(VisionMLTask task, QString const &name)
{
    if (modelName(task) == name) {
        return; // no change
    }
    QMutexLocker lock(&m_mutex);
    m_modelName[(int)task] = name;
    m_config.writeEntry(QString("model_%1").arg(toString(task)), name);
    m_config.writeEntry(QString("model_%1").arg((int)task), name);
    unloadModels();
    Q_EMIT modelNameChanged(task, name);
}

QString VisionModels::backendDeviceDescription() const
{
    ggml_backend_dev_t dev = ggml_backend_get_device(m_backend);
    char const *name = ggml_backend_dev_name(dev);
    char const *desc = ggml_backend_dev_description(dev);
    return QString("%1 [%2]").arg(QString(desc).trimmed(), name);
}

void VisionModels::unloadModels()
{
    m_sam = {};
    m_birefnet = {};
    m_migan = {};
}

void VisionModels::cleanUp()
{
    // This would run in the destructor anyway, but because the plugin manager which keeps this
    // object alive is static, it may happen too late and in arbitrary order. Dynamic libraries
    // which the plugin relies on may already be gone.
    unloadModels();
    m_backend = {};
}

//
// VisionMLImage

VisionMLImage VisionMLImage::prepare(KisPaintDevice const &device, QRect bounds)
{
    VisionMLImage result;
    if (bounds.isEmpty()) {
        bounds = device.exactBounds();
    }
    if (bounds.isEmpty()) {
        return result; // Can happen eg. when using color label mode without matching layers.
    }
    KoColorSpace const *cs = device.colorSpace();
    const KoColorSpace *srgbCs = KoColorSpaceRegistry::instance()->rgb8();
    const KoColorProfile *srgbProfile = srgbCs ? srgbCs->profile() : nullptr;
    const KoColorProfile *csProfile = cs ? cs->profile() : nullptr;
    const bool isSRGB =
        csProfile && srgbProfile && (*csProfile == *srgbProfile || csProfile->name() == srgbProfile->name());

    if (cs && cs->pixelSize() == 4 && cs->id() == "RGBA" && isSRGB) {
        // Stored as BGRA, 8 bits per channel in sRGB in Krita. Direct fast path.
        result.data = QImage(bounds.width(), bounds.height(), QImage::Format_ARGB32);
        device.readBytes(result.data.bits(), bounds);
    } else {
        // Convert everything else (non-sRGB profile, wide-gamut, 16-bit/float, CMYK/Lab/Gray)
        // to QImage::Format_ARGB32 in default color space (sRGB).
        result.data = device.convertToQImage(nullptr, bounds);
    }
    result.view.extent = {result.data.width(), result.data.height()};
    result.view.stride = result.data.bytesPerLine();
    result.view.format = visp::image_format::bgra_u8; // QImage::Format_ARGB32 is BGRA in little endian byte order
    result.view.data = result.data.bits();
    return result;
}

// Convert outputs to QImage - this is mainly because they're RGBA, but Krita paint device uses BGRA internally (but may
// also use some other color space).
QImage VisionMLImage::convertToQImage(visp::image_view const &img, QRect b)
{
    if (img.format != visp::image_format::rgba_u8) {
        throw std::runtime_error("Unsupported image format for conversion to QImage");
    }
    if (b.isEmpty()) {
        b = QRect(0, 0, img.extent[0], img.extent[1]);
    }

    QImage result(b.width(), b.height(), QImage::Format_RGBA8888);
    // copy scanlines, row stride might be different
    size_t rowSize = b.width() * n_bytes(img.format);
    size_t rowStride = img.extent[0] * n_bytes(img.format);
    size_t rowOffset = b.x() * n_bytes(img.format);
    for (int y = 0; y < b.height(); ++y) {
        memcpy(result.scanLine(y), ((uint8_t const *)img.data) + (y + b.y()) * rowStride + rowOffset, rowSize);
    }
    return result;
}

//
// VisionMLBackendWidget

VisionMLBackendWidget::VisionMLBackendWidget(QSharedPointer<VisionModels> shared, bool showDevice, QWidget *parent)
    : KisOptionCollectionWidgetWithHeader(i18n("Backend"), parent)
    , m_shared(std::move(shared))
{
    QWidget *widget = new QWidget;
    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    KisOptionButtonStrip *strip = new KisOptionButtonStrip;
    m_cpuButton = strip->addButton(i18n("CPU"));
    m_gpuButton = strip->addButton(i18n("GPU"));
    if (!visp::backend_is_available(visp::backend_type::cpu)) {
        m_cpuButton->setEnabled(false);
        m_cpuButton->setToolTip(i18n("CPU backend not available, hardware is not supported"));
    }
    if (!visp::backend_is_available(visp::backend_type::gpu)) {
        m_gpuButton->setEnabled(false);
        m_gpuButton->setToolTip(i18n("GPU backend not available, no supported devices found"));
    }
    layout->addWidget(strip);

    if (showDevice) {
        m_deviceLabel = new QLabel;
        layout->addWidget(m_deviceLabel);
    }

    setPrimaryWidget(widget);

    connect(strip, SIGNAL(buttonToggled(KoGroupButton *, bool)), this, SLOT(switchBackend(KoGroupButton *, bool)));
    connect(m_shared.get(), SIGNAL(backendChanged(visp::backend_type)), this, SLOT(updateBackend(visp::backend_type)));

    updateBackend(m_shared->backend());
}

void VisionMLBackendWidget::updateBackend(visp::backend_type backend)
{
    m_cpuButton->setChecked(backend == visp::backend_type::cpu);
    m_gpuButton->setChecked(backend == visp::backend_type::gpu);

    if (m_deviceLabel) {
        m_deviceLabel->setText(QString(m_shared->backendDeviceDescription()).trimmed());
    }
}

void VisionMLBackendWidget::switchBackend(KoGroupButton *button, bool checked)
{
    if (checked) {
        bool success = m_shared->setBackend(button == m_cpuButton ? visp::backend_type::cpu : visp::backend_type::gpu);
        if (!success) {
            button->setEnabled(false);
            KoGroupButton *prev = m_shared->backend() == visp::backend_type::cpu ? m_cpuButton : m_gpuButton;
            bool blocked = prev->blockSignals(true);
            prev->setChecked(true);
            prev->blockSignals(blocked);
        }
    }
}

//
// VisionMLModelSelect

VisionMLModelSelect::VisionMLModelSelect(QSharedPointer<VisionModels> models,
                                         VisionMLTask task,
                                         bool showFolderButton,
                                         QWidget *parent)
    : KisOptionCollectionWidgetWithHeader(i18n("Model"), parent)
    , m_shared(std::move(models))
    , m_task(task)
{
    QWidget *widget = new QWidget;
    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    m_select = new QComboBox;
    updateModels();
    updateModel(m_task, m_shared->modelName(m_task));
    connect(m_select, SIGNAL(currentIndexChanged(int)), this, SLOT(switchModel(int)));
    connect(m_shared.get(), &VisionModels::modelNameChanged, this, &VisionMLModelSelect::updateModel);
    layout->addWidget(m_select);

    if (showFolderButton) {
        QToolButton *folderButton = new QToolButton;
        folderButton->setIcon(KisIconUtils::loadIcon("document-open"));
        folderButton->setFixedSize(24, 24);
        folderButton->setToolTip(i18n("Open models folder"));
        connect(folderButton, &QToolButton::clicked, this, &VisionMLModelSelect::openModelsFolder);
        layout->addWidget(folderButton);
    }

    // Offer the optional higher-quality BiRefNet Dynamic weights when the
    // task is background removal and the file is not yet on disk.
    if (m_task == VisionMLTask::background_removal) {
        m_downloadButton = new QToolButton;
        m_downloadButton->setIcon(KisIconUtils::loadIcon("download"));
        m_downloadButton->setFixedSize(24, 24);
        m_downloadButton->setToolTip(i18n("Download the optional higher-quality BiRefNet Dynamic model"));
        connect(m_downloadButton, &QToolButton::clicked, this, &VisionMLModelSelect::downloadModel);
        layout->addWidget(m_downloadButton);
        m_downloadButton->setVisible(resolveModelPath(QStringLiteral("birefnet/BiRefNet-dynamic-F16.gguf")).isEmpty());
    }

    m_fileWatcher = new QFileSystemWatcher(this);
    m_fileWatcher->addPath(findModelPath(m_task));
    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, &VisionMLModelSelect::updateModels);

    setPrimaryWidget(widget);
}

void VisionMLModelSelect::updateModels()
{
    m_select->blockSignals(true);

    QVariant current = m_select->currentData();
    m_select->clear();

    auto addModels = [this](const QString &arch) {
        QSet<QString> added;
        for (const QString &root : modelRoots()) {
            QDir modelDir(QDir(root).filePath(arch));
            const QStringList modelFiles = modelDir.entryList(QStringList() << "*.gguf", QDir::Files);
            for (const QString &file : modelFiles) {
                const QString fullName = arch + "/" + file;
                if (!added.contains(fullName)) {
                    m_select->addItem(QFileInfo(file).completeBaseName(), fullName);
                    added.insert(fullName);
                }
            }
        }
    };

    switch (m_task) {
    case VisionMLTask::segmentation:
        addModels("sam");
        break;
    case VisionMLTask::background_removal:
        addModels("birefnet");
        break;
    case VisionMLTask::inpainting:
        addModels("migan");
        break;
    default:
        qWarning() << "Unknown VisionMLTask" << (int)m_task;
        return;
    }

    m_select->blockSignals(false);
    if (current.isValid()) {
        updateModel(m_task, current.toString());
    }
}

void VisionMLModelSelect::switchModel(int index)
{
    if (index < 0 || index >= m_select->count()) {
        return;
    }
    QString modelName = m_select->itemData(index).toString();
    m_shared->setModelName(m_task, modelName);
}

void VisionMLModelSelect::updateModel(VisionMLTask task, QString const &name)
{
    if (m_task != task) {
        return; // not the model we are interested in
    }
    int index = m_select->findData(name);
    if (index != -1) {
        m_select->setCurrentIndex(index);
    } else {
        m_select->setCurrentIndex(0); // Fallback to the first model if the current one is not found
    }
}

void VisionMLModelSelect::openModelsFolder()
{
    QString folder = findModelPath(m_task);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
        QMessageBox::warning(nullptr,
                             i18nc("@title:window", "Solstice - Vision ML"),
                             i18n("Failed to open folder: ") + folder);
    }
}

void VisionMLModelSelect::downloadModel()
{
    // Only the background-removal task has an optional download so far:
    // the higher-quality BiRefNet Dynamic weights (~900 MB) from the same
    // Hugging Face repository the bundled Lite weights come from.
    const QString relativeName = QStringLiteral("birefnet/BiRefNet-dynamic-F16.gguf");
    const QUrl url(QStringLiteral("https://huggingface.co/Acly/BiRefNet-GGUF/resolve/main/BiRefNet-dynamic-F16.gguf"));

    // KisRemoteFileFetcher shows its own confirmation + progress dialog.
    const QString folder = findModelPath(m_task);
    QDir().mkpath(folder);
    const QString outPath = QDir(folder).filePath(QFileInfo(relativeName).fileName());
    // Download to a .part file and rename on success so a crash mid-download
    // cannot leave a corrupt model that resolveModelPath would pick up.
    const QString partPath = outPath + QStringLiteral(".part");
    QFile out(partPath);
    if (!out.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this,
                             i18nc("@title:window", "Solstice - Vision ML"),
                             i18n("Cannot write model file: ") + outPath);
        return;
    }

    KisRemoteFileFetcher fetcher;
    const bool ok = fetcher.fetchFile(url, &out);
    out.close();
    if (!ok || !QFile::rename(partPath, outPath)) {
        out.remove(); // don't leave a partial file behind
        if (ok) {
            QMessageBox::warning(this,
                                 i18nc("@title:window", "Solstice - Vision ML"),
                                 i18n("Cannot move downloaded model into place: ") + outPath);
        } else {
            QMessageBox::warning(this,
                                 i18nc("@title:window", "Solstice - Vision ML"),
                                 i18n("Model download failed: ") + url.toString());
        }
        return;
    }

    // The filesystem watcher refreshes the list; select the new model
    // and hide the offer.
    m_shared->setModelName(m_task, relativeName);
    if (m_downloadButton) {
        m_downloadButton->setVisible(false);
    }
}

//
// VisionMLErrorReporter

VisionMLErrorReporter::VisionMLErrorReporter(QObject *parent)
    : QObject(parent)
{
    connect(this, &VisionMLErrorReporter::errorOccurred, this, &VisionMLErrorReporter::showError, Qt::QueuedConnection);
}

void VisionMLErrorReporter::showError(QString const &message) const
{
    QMessageBox::warning(nullptr,
                         i18nc("@title:window", "Solstice - Vision ML"),
                         i18n("Error during image processing: ") + message);
}
