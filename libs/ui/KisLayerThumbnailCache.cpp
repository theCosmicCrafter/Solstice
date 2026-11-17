/*
 *  SPDX-FileCopyrightText: 2023 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisLayerThumbnailCache.h"

#include "kis_config.h"
#include "kis_image.h"
#include "KisIdleTasksManager.h"
#include "kis_layer_utils.h"

#include "KisRunnableStrokeJobUtils.h"
#include "KisRunnableStrokeJobsInterface.h"

#include <QFutureWatcher>
#include <QtConcurrent>

#include <limits>

namespace {
// Solstice: the sequence number of a provisional thumbnail. It never equals a
// node's thumbnailSeqNo() (KisBaseNode returns -1), so the idle task always
// regenerates it.
constexpr int ProvisionalSeqNo = std::numeric_limits<int>::min();

struct ThumbnailRecord {
    QImage image;
    int seqNo = -1;
    int maxSize = 0;
    // Solstice: the node is tracked here, not in the map key. A KisNodeWSP
    // key compares as nullptr once its node is destroyed, which breaks the
    // map's ordering and hides the records of living nodes.
    KisNodeWSP node;
};

// Records are keyed by the node's address. The address may be reused by a
// new node after the old one is destroyed, so a record only belongs to a
// node while its weak pointer is still valid and points to that node.
using ThumbnailMap = QMap<const KisNode *, ThumbnailRecord>;

ThumbnailMap::const_iterator findRecord(const ThumbnailMap &cache, KisNodeSP node)
{
    auto it = cache.constFind(node.data());
    if (it != cache.constEnd() && (!it->node.isValid() || it->node != node.data())) {
        it = cache.constEnd();
    }
    return it;
}
} // namespace

struct ThumbnailsStroke : KisIdleTaskStrokeStrategy
{
    Q_OBJECT
public:
    ThumbnailsStroke(KisImageSP image, int maxSize, const ThumbnailMap &cache)
        : KisIdleTaskStrokeStrategy(QLatin1String("layer-thumbnails-stroke"), kundo2_i18n("Update layer thumbnails"))
        , m_root(image->root())
        , m_maxSize(maxSize)
        , m_cache(cache)
    {
        // thread-safety!
        m_cache.detach();
    }

    void initStrokeCallback() override
    {
        KisIdleTaskStrokeStrategy::initStrokeCallback();

        using KisLayerUtils::recursiveApplyNodes;
        using KritaUtils::addJobConcurrent;

        QVector<KisRunnableStrokeJobData*> jobs;
        recursiveApplyNodes(m_root, [&jobs, this] (KisNodeSP node) {


            if (!node->parent()) return;
            if (node->isFakeNode()) return;

            bool shouldRegenerateThumbnail = false;

            auto it = findRecord(m_cache, node);

            if (it != m_cache.constEnd()) {
                ThumbnailRecord rec = *it;

                if (rec.seqNo != node->thumbnailSeqNo() ||
                    rec.maxSize != m_maxSize) {

                    shouldRegenerateThumbnail = true;
                }
            } else {
                shouldRegenerateThumbnail = true;
            }

            if (shouldRegenerateThumbnail) {
                const int timeout = KisConfig(true).layerThumbnailGenerationTimeout();

                addJobConcurrent(jobs, [node, timeout, this] () mutable {
                    QElapsedTimer timer;
                    timer.start();
                    QImage image = node->createPreferredThumbnail(m_maxSize, m_maxSize, Qt::KeepAspectRatio);
                    const int measuredTime = timer.elapsed();

                    if (node->preferredThumbnailBoundsMode() == KisThumbnailBoundsMode::Precise &&
                        measuredTime > timeout) {
                        warnUI << "WARNING: thumbnail generation for" << node->name() << "took longer than expected:" << measuredTime << "(timeout:" << timeout << ")";
                        warnUI << "         This layer's thumbnail will be rendered in imprecise mode from now on";
                        node->setPreferredThumbnailBoundsMode(KisThumbnailBoundsMode::Coarse);
                    }

                    this->sigThumbnailGenerated(node, node->thumbnailSeqNo(), m_maxSize, image);
                });
            }
        });

        runnableJobsInterface()->addRunnableJobs(jobs);
    }

Q_SIGNALS:
    void sigThumbnailGenerated(KisNodeSP node, int maxSize, int seqNo, const QImage &thumb);
private:

    KisNodeSP m_root;
    int m_maxSize;
    ThumbnailMap m_cache;
};

struct KisLayerThumbnailCache::Private
{
    KisImageWSP image;

    KisIdleTasksManager::TaskGuard taskGuard;
    int maxSize = 32;
    ThumbnailMap cache;

    void cleanupDeletedNodes();
};


KisLayerThumbnailCache::KisLayerThumbnailCache()
    : m_d(new Private())
{
}

KisLayerThumbnailCache::~KisLayerThumbnailCache() = default;

void KisLayerThumbnailCache::setIdleTaskManagerImpl(KisIdleTasksManager *manager)
{
    if (manager) {
        m_d->taskGuard = manager->addIdleTaskWithGuard([this] (KisImageSP image) {
            ThumbnailsStroke *stroke = new ThumbnailsStroke(image, m_d->maxSize, m_d->cache);
            connect(stroke, SIGNAL(sigThumbnailGenerated(KisNodeSP, int, int, QImage)), this, SLOT(slotThumbnailGenerated(KisNodeSP, int, int, QImage)));
            return stroke;
        });
    } else {
        m_d->taskGuard = KisIdleTasksManager::TaskGuard();
    }
}

void KisLayerThumbnailCache::setImage(KisImageSP image)
{
    m_d->image = image;
    m_d->cache.clear();

    if (m_d->image && m_d->taskGuard.isValid()) {
        m_d->taskGuard.trigger();
    }
}

void KisLayerThumbnailCache::setIdleTaskManager(KisIdleTasksManager *manager)
{
    setIdleTaskManagerImpl(manager);
    if (m_d->image && m_d->taskGuard.isValid()) {
        m_d->taskGuard.trigger();
    }
}

void KisLayerThumbnailCache::setImage(KisImageSP image, KisIdleTasksManager *manager)
{
    setIdleTaskManagerImpl(manager);
    setImage(image);
    // the update is triggered only by the second call, not the first one!
}

void KisLayerThumbnailCache::setMaxSize(int maxSize)
{
    m_d->maxSize = maxSize;
    if (m_d->image && m_d->taskGuard.isValid()) {
        m_d->taskGuard.trigger();
    }
}

int KisLayerThumbnailCache::maxSize() const
{
    return m_d->maxSize;
}

QImage KisLayerThumbnailCache::thumbnail(KisNodeSP node) const
{
    QImage image;

    auto it = findRecord(m_d->cache, node);
    if (it != m_d->cache.constEnd()) {
        image = it->image;

        if (it->maxSize > m_d->maxSize) {
            image = image.scaled(m_d->maxSize, m_d->maxSize, Qt::KeepAspectRatio);
        }
    } else {
        image = QImage(1, 1, QImage::Format_ARGB32);
        image.fill(0);
    }

    return image;
}

void KisLayerThumbnailCache::Private::cleanupDeletedNodes()
{
    for (auto it = cache.begin(); it != cache.end();) {
        if (!it->node.isValid()) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

void KisLayerThumbnailCache::notifyNodeRemoved(KisNodeSP node)
{
    Q_UNUSED(node);
    m_d->cleanupDeletedNodes();
}

void KisLayerThumbnailCache::notifyNodeAdded(KisNodeSP node)
{
    m_d->cleanupDeletedNodes();
    startProvisionalThumbnail(node);

    if (m_d->image && m_d->taskGuard.isValid()) {
        m_d->taskGuard.trigger();
    }
}

void KisLayerThumbnailCache::startProvisionalThumbnail(KisNodeSP node)
{
    /**
     * Solstice: an added node has no thumbnail until the idle task runs, and
     * that waits until the image is idle. After Duplicate Layer the node
     * juggler keeps its stroke open for seconds (to merge repeated actions
     * into one undo step), so the Layers docker showed an empty thumbnail
     * meanwhile. Draw a provisional thumbnail right away on a worker thread,
     * like the Overview's live update reads the image during strokes. It is
     * stored with ProvisionalSeqNo, so the idle task still regenerates it
     * (e.g. a duplicated group whose projection was still being updated).
     */
    if (!node || !node->parent() || node->isFakeNode())
        return;
    if (!m_d->image || node->image() != m_d->image)
        return;
    if (findRecord(m_d->cache, node) != m_d->cache.constEnd())
        return;

    const int maxSize = m_d->maxSize;
    KisNodeWSP weakNode = node;
    QFutureWatcher<QImage> *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, weakNode, maxSize]() {
        watcher->deleteLater();
        KisNodeSP node = weakNode.toStrongRef();
        const QImage image = watcher->result();
        if (!node || image.isNull() || maxSize != m_d->maxSize || node->image() != m_d->image)
            return;

        // A thumbnail from the idle task is newer: keep it.
        auto it = findRecord(m_d->cache, node);
        if (it != m_d->cache.constEnd() && it->seqNo != ProvisionalSeqNo)
            return;

        m_d->cache[node.data()] = {image, ProvisionalSeqNo, maxSize, node};
        Q_EMIT sigLayerThumbnailUpdated(node);
    });
    watcher->setFuture(QtConcurrent::run([node, maxSize]() mutable {
        return node->createPreferredThumbnail(maxSize, maxSize, Qt::KeepAspectRatio);
    }));
}

void KisLayerThumbnailCache::clear()
{
    m_d->cache.clear();
}

void KisLayerThumbnailCache::slotThumbnailGenerated(KisNodeSP node, int seqNo, int maxSize, const QImage &thumb)
{
    if (node->image() != m_d->image) {
        qWarning() << "KisLayerThumbnailCache::slotThumbnailGenerated: node does not belong to the attached image anymore!" << ppVar(node) << ppVar(m_d->image);
        return;
    }

    m_d->cache[node.data()] = {thumb, seqNo, maxSize, node};
    Q_EMIT sigLayerThumbnailUpdated(node);
}

#include "KisLayerThumbnailCache.moc"
