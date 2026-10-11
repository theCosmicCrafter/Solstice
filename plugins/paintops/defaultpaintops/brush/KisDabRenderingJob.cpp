/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisDabRenderingJob.h"
#include "KisPaintTrace.h"

#include <QElapsedTimer>

#include <atomic>

#include <KisRunnableStrokeJobsInterface.h>
#include <KisRunnableStrokeJobData.h>

#include "KisBrushOpResources.h"
#include "KisDabCacheUtils.h"
#include "KisDabRenderingQueue.h"
#include "KisSharpnessOption.h"
#include "kis_texture_option.h"

#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <kis_auto_brush.h>

#include "gpu/KisGpuBrushPainter.h"

#include <tool/strokes/FreehandStrokeRunnableJobDataWithUpdate.h>

KisDabRenderingJob::KisDabRenderingJob(int _seqNo, KisDabRenderingJob::JobType _type, qreal _opacity, qreal _flow)
    : seqNo(_seqNo)
    , type(_type)
    , opacity(_opacity)
    , flow(_flow)
{
}

KisDabRenderingJob::KisDabRenderingJob(const KisDabRenderingJob &rhs)
    : seqNo(rhs.seqNo)
    , paintTraceId(rhs.paintTraceId)
    , generationInfo(rhs.generationInfo)
    , type(rhs.type)
    , originalDevice(rhs.originalDevice)
    , postprocessedDevice(rhs.postprocessedDevice)
    , procedural(rhs.procedural)
    , proceduralFlips(rhs.proceduralFlips)
    , pixelsPending(rhs.pixelsPending)
    , status(rhs.status)
    , opacity(rhs.opacity)
    , flow(rhs.flow)
{
}

KisDabRenderingJob &KisDabRenderingJob::operator=(const KisDabRenderingJob &rhs)
{
    seqNo = rhs.seqNo;
    paintTraceId = rhs.paintTraceId;
    generationInfo = rhs.generationInfo;
    type = rhs.type;
    originalDevice = rhs.originalDevice;
    postprocessedDevice = rhs.postprocessedDevice;
    procedural = rhs.procedural;
    proceduralFlips = rhs.proceduralFlips;
    pixelsPending = rhs.pixelsPending;
    status = rhs.status;
    opacity = rhs.opacity;
    flow = rhs.flow;

    return *this;
}

QPoint KisDabRenderingJob::dstDabOffset() const
{
    /// Recenter generated low-res dab around the center
    /// of the ideal theoretical dab rect
    const QPoint p1 = generationInfo.dstDabRect.topLeft();
    const QPoint s1 = QPoint(generationInfo.dstDabRect.width(),
                             generationInfo.dstDabRect.height());
    const QPoint s2 = QPoint(postprocessedDevice->bounds().width(),
                             postprocessedDevice->bounds().height());
    return p1 + (s1 - s2) / 2;
}



namespace
{
/// GPU engine (Solstice, phase 4.85): skip the CPU generation of a mask
/// kind only after this many of its dabs matched their description exactly
/// in this process; any mismatch disables skipping for that kind.
constexpr int VerifiedDabsBeforeSkipping = 16;
// circle kinds 0-2, then image tips by application (3 + application)
constexpr int KindCount = 7;
std::atomic<int> s_verifiedDabs[KindCount] = {};
std::atomic<bool> s_mismatch[KindCount] = {};
std::atomic<quint64> s_skippedGenerations{0};
std::atomic<quint64> s_texturedDescriptions{0};

int kindIndex(const KisProceduralCircleDab &circle)
{
    return circle.kind == KisProceduralCircleDab::GaussCircle ? 1
        : circle.kind == KisProceduralCircleDab::SoftCircle   ? 2
        : circle.kind == KisProceduralCircleDab::ImageTip     ? 3 + int(qMin<quint32>(circle.imageApplication, 3))
                                                              : 0;
}

quint32 mirrorFlips(const KisDabCacheUtils::DabGenerationInfo &di)
{
    return (di.mirrorProperties.horizontalMirror ? 1u : 0u) | (di.mirrorProperties.verticalMirror ? 2u : 0u);
}

/// Whether the dabs of @p resources may be described for the GPU. Solstice:
/// the brush op decides once per stroke (KisBrushOpResources::gpuDabs);
/// KisGpuBrushPainter::isEnabled() reads the environment, which for every
/// dab contended a global lock across the dab threads.
bool gpuDabsWanted(KisDabCacheUtils::DabRenderingResources *resources)
{
    const auto *brushResources = dynamic_cast<const KisBrushOpResources *>(resources);
    return brushResources ? brushResources->gpuDabs : KisGpuBrushPainter::isEnabled();
}

/// GPU engine (Solstice, "Textured dabs"): the GPU description of the
/// texture of the dab @p di, when the pattern texture is its only
/// postprocessing and can run on the GPU (alpha-only modes, RGBA F32).
QSharedPointer<const KisProceduralDabTexture> gpuTexture(const KisDabCacheUtils::DabGenerationInfo &di,
                                                         KisDabCacheUtils::DabRenderingResources *resources,
                                                         const KoColorSpace *dabColorSpace)
{
    if (!di.needsPostprocessing || !resources->textureOption || !resources->textureOption->m_enabled
        || (resources->sharpnessOption && resources->sharpnessOption->isChecked())) {
        return {};
    }
    return resources->textureOption->gpuDescription(di.dstDabRect.topLeft(), di.info, dabColorSpace, mirrorFlips(di));
}

/// The GPU description of the dab @p di generates, or null when the dab is
/// not an RGBA F32 vectorized circle auto-brush dab (or RGBA F16), or an
/// RGBA F32 image tip dab ("Image tip dabs"). A dab whose only
/// postprocessing is a GPU texture (gpuTexture()) is described too; its
/// description is the circle before texturing.
QSharedPointer<KisProceduralCircleDab> buildCircleDab(const KisDabCacheUtils::DabGenerationInfo &di,
                                                      KisDabCacheUtils::DabRenderingResources *resources,
                                                      const KisFixedPaintDeviceSP &dab)
{
    if (!gpuDabsWanted(resources) || !di.solidColorFill || !dab || !resources->brush) {
        return {};
    }
    if (di.needsPostprocessing && !gpuTexture(di, resources, dab->colorSpace())) {
        return {};
    }
    const KoColorSpace *cs = dab->colorSpace();
    const bool f32 = cs->colorDepthId() == Float32BitsColorDepthID && cs->pixelSize() == 16;
    const bool f16 = cs->colorDepthId() == Float16BitsColorDepthID && cs->pixelSize() == 8;
    if (cs->colorModelId() != RGBAColorModelID || (!f32 && !f16) || !di.paintColor.colorSpace()
        || *di.paintColor.colorSpace() != *cs) {
        return {};
    }
    const auto *autoBrush = dynamic_cast<const KisAutoBrush *>(resources->brush.data());
    QSharedPointer<KisProceduralCircleDab> circle(new KisProceduralCircleDab());
    if (!autoBrush) {
        // An image tip (predefined brush), RGBA F32 only
        if (!f32
            || !resources->brush->proceduralImageDab(di.shape,
                                                     di.subPixel.x(),
                                                     di.subPixel.y(),
                                                     di.paintColor.data(),
                                                     cs,
                                                     di.lightnessStrength,
                                                     circle.data())) {
            return {};
        }
        return circle;
    }
    if (!autoBrush->proceduralCircleDab(di.shape,
                                        di.info,
                                        di.subPixel.x(),
                                        di.subPixel.y(),
                                        di.softnessFactor,
                                        di.paintColor.data(),
                                        circle.data(),
                                        f16)) {
        return {};
    }
    return circle;
}

/**
 * GPU engine (Solstice): describes a freshly generated dab for GPU
 * evaluation (KisProceduralCircleDab), or returns null. The description is
 * checked against the generated pixels along the middle row and column, so
 * a different CPU path (scalar applicator, other generator) is never
 * described.
 */
QSharedPointer<const KisProceduralCircleDab> describeCircleDab(const KisDabCacheUtils::DabGenerationInfo &di,
                                                               KisDabCacheUtils::DabRenderingResources *resources,
                                                               KisFixedPaintDeviceSP dab,
                                                               quint32 *flips)
{
    QSharedPointer<KisProceduralCircleDab> circle = buildCircleDab(di, resources, dab);
    if (!circle) {
        return {};
    }

    const QRect bounds = dab->bounds();
    if (bounds.isEmpty() || bounds.topLeft() != QPoint()
        || (circle->imageTip && circle->imageTip->size != bounds.size())) {
        return {};
    }
    const quint32 mirror = mirrorFlips(di);
    const int width = bounds.width();
    const int height = bounds.height();
    auto matches = [&](int x, int y) {
        const int gx = (mirror & 1) ? width - 1 - x : x;
        const int gy = (mirror & 2) ? height - 1 - y : y;
        // Exact: the fades have thresholds, and any other CPU kernel (one
        // without the fused multiply-adds, a scalar path) must be rejected.
        // NaN only at the degenerate n == normFade == 1 point; refuse it.
        return circle->matchesPixel(dab->constData(), width, x, y, gx, gy);
    };
    bool exact = true;
    for (int x = 0; exact && x < width; x++) {
        exact = matches(x, height / 2);
    }
    for (int y = 0; exact && y < height; y++) {
        exact = matches(width / 2, y);
    }
    if (!exact) {
        s_mismatch[kindIndex(*circle)] = true;
        return {};
    }
    s_verifiedDabs[kindIndex(*circle)]++;
    *flips = mirror;
    return circle;
}

/**
 * GPU engine (Solstice, phase 4.85): when the kind of the dab is verified,
 * sizes @p dab like the CPU generator would and returns its description
 * without generating the pixels (KisRenderedDab::pixelsPending). Null means
 * generate the dab normally.
 */
QSharedPointer<const KisProceduralCircleDab>
describeWithoutPixels(const KisDabCacheUtils::DabGenerationInfo &di,
                      KisDabCacheUtils::DabRenderingResources *resources,
                      KisFixedPaintDeviceSP dab,
                      quint32 *flips)
{
    if (!gpuDabsWanted(resources)) {
        return {};
    }
    QSharedPointer<KisProceduralCircleDab> circle = buildCircleDab(di, resources, dab);
    if (!circle) {
        return {};
    }
    const int kind = kindIndex(*circle);
    if (s_mismatch[kind] || s_verifiedDabs[kind] < VerifiedDabsBeforeSkipping) {
        return {};
    }
    const int width =
        resources->brush->maskWidth(di.shape, di.subPixel.x(), di.subPixel.y(), di.info);
    const int height =
        resources->brush->maskHeight(di.shape, di.subPixel.x(), di.subPixel.y(), di.info);
    if (width <= 0 || height <= 0 || (circle->imageTip && circle->imageTip->size != QSize(width, height))) {
        return {};
    }
    // The same bounds as generateMaskAndApplyMaskOrCreateDab(); the buffer
    // stays allocated so that CPU reflections of pending pixels are safe.
    dab->setRect(QRect(0, 0, width, height));
    dab->lazyGrowBufferWithoutInitialization();
    *flips = mirrorFlips(di);
    return circle;
}
} // namespace

quint64 KisDabRenderingJobRunner::skippedGenerationCount()
{
    return s_skippedGenerations.load();
}

quint64 KisDabRenderingJobRunner::texturedDescriptionCount()
{
    return s_texturedDescriptions.load();
}

KisDabRenderingJobRunner::KisDabRenderingJobRunner(KisDabRenderingJobSP job,
                                                   KisDabRenderingQueue *parentQueue,
                                                   KisRunnableStrokeJobsInterface *runnableJobsInterface)
    : m_job(job),
      m_parentQueue(parentQueue),
      m_runnableJobsInterface(runnableJobsInterface)
{
}

KisDabRenderingJobRunner::~KisDabRenderingJobRunner()
{
}

int KisDabRenderingJobRunner::executeOneJob(KisDabRenderingJob *job,
                                            KisDabCacheUtils::DabRenderingResources *resources,
                                            KisDabRenderingQueue *parentQueue)
{
    using namespace KisDabCacheUtils;
    KisPaintTrace::Scope trace("dab.generate_and_postprocess", parentQueue, job, job->paintTraceId);

    KIS_SAFE_ASSERT_RECOVER_NOOP(job->type == KisDabRenderingJob::Dab ||
                                 job->type == KisDabRenderingJob::Postprocess);

    QElapsedTimer executionTime;
    executionTime.start();

    resources->syncResourcesToSeqNo(job->seqNo, job->generationInfo.info);

    bool generated = false;
    if (job->type == KisDabRenderingJob::Dab) {
        // TODO: thing about better interface for the reverse queue link
        job->originalDevice = parentQueue->fetchCachedPaintDevice();

        quint32 flips = 0;
        QSharedPointer<const KisProceduralCircleDab> pending =
            describeWithoutPixels(job->generationInfo, resources, job->originalDevice, &flips);
        if (pending) {
            job->procedural = pending;
            job->proceduralFlips = flips;
            job->pixelsPending = true;
            s_skippedGenerations++;
            KisPaintTrace::link("dab.generation_skipped", parentQueue, job->paintTraceId);
        } else {
            job->pixelsPending = false;
            generateDab(job->generationInfo, resources, &job->originalDevice);
            generated = true;
        }
    }

    // by now the original device should be already prepared
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(job->originalDevice, 0);

    if (job->type == KisDabRenderingJob::Dab ||
        job->type == KisDabRenderingJob::Postprocess) {

        if (job->generationInfo.needsPostprocessing) {
            // TODO: cache postprocessed device

            if (!job->postprocessedDevice ||
                *job->originalDevice->colorSpace() != *job->postprocessedDevice->colorSpace()) {

                job->postprocessedDevice = parentQueue->fetchCachedPaintDevice();
                *job->postprocessedDevice = *job->originalDevice;
            } else {
                *job->postprocessedDevice = *job->originalDevice;
            }

            // GPU engine (Solstice, "Textured dabs"): a pending dab whose
            // texture runs on the GPU stays pending; its description is the
            // circle with this dab's texture.
            QSharedPointer<const KisProceduralDabTexture> texture;
            if (job->pixelsPending && job->procedural) {
                texture = gpuTexture(job->generationInfo, resources, job->postprocessedDevice->colorSpace());
            }
            if (texture) {
                QSharedPointer<KisProceduralCircleDab> textured(new KisProceduralCircleDab(*job->procedural));
                textured->texture = texture;
                job->procedural = textured;
                job->proceduralFlips = texture->brushFlips;
                s_texturedDescriptions++;
                return executionTime.nsecsElapsed() / 1000;
            }

            // GPU engine (Solstice): the shared original of a pending dab
            // has no pixels; render them into this job's own copy.
            if (job->pixelsPending && job->procedural) {
                job->postprocessedDevice->lazyGrowBufferWithoutInitialization();
                job->procedural->render(job->postprocessedDevice->data(),
                                        job->postprocessedDevice->bounds().width(),
                                        job->postprocessedDevice->bounds().height(),
                                        job->proceduralFlips & 3);
            }

            postProcessDab(job->postprocessedDevice,
                           job->generationInfo.dstDabRect.topLeft(),
                           job->generationInfo.info,
                           resources);

            // The postprocessed pixels are not described.
            job->procedural.reset();
            job->proceduralFlips = 0;
            job->pixelsPending = false;
        } else {
            job->postprocessedDevice = job->originalDevice;
        }
    }

    if (generated) {
        job->proceduralFlips = 0;
        if (job->generationInfo.needsPostprocessing) {
            // Textured: verify the circle against the original pixels (the
            // kind then skips CPU generation); the CPU-textured pixels are
            // not described.
            quint32 flips = 0;
            describeCircleDab(job->generationInfo, resources, job->originalDevice, &flips);
        } else {
            job->procedural =
                describeCircleDab(job->generationInfo, resources, job->postprocessedDevice, &job->proceduralFlips);
        }
    }

    return executionTime.nsecsElapsed() / 1000;
}

void KisDabRenderingJobRunner::run()
{
    int executionTime = 0;

    KisDabCacheUtils::DabRenderingResources *resources = m_parentQueue->fetchResourcesFromCache();

    executionTime = executeOneJob(m_job.data(), resources, m_parentQueue);
    QList<KisDabRenderingJobSP> jobs = m_parentQueue->notifyJobFinished(m_job->seqNo, executionTime);

    while (!jobs.isEmpty()) {
        QVector<KisRunnableStrokeJobData*> dataList;

        // start all-but-the-first jobs asynchronously
        for (int i = 1; i < jobs.size(); i++) {
            dataList.append(new FreehandStrokeRunnableJobDataWithUpdate(
                                new KisDabRenderingJobRunner(jobs[i], m_parentQueue, m_runnableJobsInterface),
                                KisStrokeJobData::CONCURRENT));
        }

        m_runnableJobsInterface->addRunnableJobs(dataList);


        // execute the first job in the current thread
        KisDabRenderingJobSP job = jobs.first();
        executionTime = executeOneJob(job.data(), resources, m_parentQueue);
        jobs = m_parentQueue->notifyJobFinished(job->seqNo, executionTime);
    }

    m_parentQueue->putResourcesToCache(resources);
}
