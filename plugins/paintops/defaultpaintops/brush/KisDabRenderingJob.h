/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISDABRENDERINGJOB_H
#define KISDABRENDERINGJOB_H

#include <QRunnable>
#include <KisDabCacheUtils.h>
#include <kis_fixed_paint_device.h>
#include <kis_types.h>
#include <KisProceduralCircleDab.h>
#include <QSharedPointer>
#include "kritadefaultpaintops_export.h"

class KisDabRenderingQueue;
class KisRunnableStrokeJobsInterface;

class KRITADEFAULTPAINTOPS_EXPORT KisDabRenderingJob
{
public:
    enum JobType {
        Dab,
        Postprocess,
        Copy
    };

    enum Status {
        New,
        Running,
        Completed
    };

public:
    KisDabRenderingJob(int _seqNo,
                       JobType _type, qreal opacity = OPACITY_OPAQUE_F, qreal flow = OPACITY_OPAQUE_F);
    KisDabRenderingJob(const KisDabRenderingJob &rhs);
    KisDabRenderingJob& operator=(const KisDabRenderingJob &rhs);

    QPoint dstDabOffset() const;

    int seqNo = -1;
    quint64 paintTraceId = 0; // logical dab request, including cache hits
    KisDabCacheUtils::DabGenerationInfo generationInfo;
    JobType type = Dab;
    KisFixedPaintDeviceSP originalDevice;
    KisFixedPaintDeviceSP postprocessedDevice;
    /// GPU engine (Solstice): description of postprocessedDevice for GPU
    /// evaluation, and the mirroring of its pixels (KisRenderedDab).
    QSharedPointer<const KisProceduralCircleDab> procedural;
    quint32 proceduralFlips = 0;
    /// The CPU generation was skipped: the devices have bounds, not pixels.
    bool pixelsPending = false;

    // high-level members, not directly related to job execution itself
    Status status = New;

    qreal opacity = OPACITY_OPAQUE_F;
    qreal flow = OPACITY_OPAQUE_F;
};

#include <QSharedPointer>
typedef QSharedPointer<KisDabRenderingJob> KisDabRenderingJobSP;

class KRITADEFAULTPAINTOPS_EXPORT KisDabRenderingJobRunner : public QRunnable
{
public:
    KisDabRenderingJobRunner(KisDabRenderingJobSP job,
                             KisDabRenderingQueue *parentQueue,
                             KisRunnableStrokeJobsInterface *runnableJobsInterface);
    ~KisDabRenderingJobRunner();

    void run() override;

    static int executeOneJob(KisDabRenderingJob *job, KisDabCacheUtils::DabRenderingResources *resources, KisDabRenderingQueue *parentQueue);

    /// GPU engine (Solstice): dabs whose CPU generation was skipped (tests).
    static quint64 skippedGenerationCount();
    /// Solstice: pending dabs described with their pattern texture (GPU).
    static quint64 texturedDescriptionCount();

private:
    KisDabRenderingJobSP m_job;
    KisDabRenderingQueue *m_parentQueue = 0;
    KisRunnableStrokeJobsInterface *m_runnableJobsInterface = 0;
};


#endif // KISDABRENDERINGJOB_H
