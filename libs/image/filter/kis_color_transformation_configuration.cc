/*
 *  SPDX-FileCopyrightText: 2015 Thorsten Zachmann <zachmann@kde.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "filter/kis_color_transformation_configuration.h"

#include <QMutex>
#include <QMutexLocker>
#include <QMap>
#include <QHash>
#include <QThread>
#include "filter/kis_color_transformation_filter.h"

struct Q_DECL_HIDDEN KisColorTransformationConfiguration::Private {
    Private()
    {}

    ~Private()
    {
        destroyCache();
    }

    void destroyCache()
    {
        QMutexLocker locker(&mutex);
        qDeleteAll(colorTransformation);
        colorTransformation.clear();
        gpuColorTransforms.clear();
    }

    // XXX: Threadlocal storage!!!
    QMap<QThread*, KoColorTransformation*> colorTransformation;
    // Solstice: GPU forms of the filter per color space, null ones included
    QHash<const KoColorSpace*, KisGpuColorTransformSP> gpuColorTransforms;
    QMutex mutex;
};

KisColorTransformationConfiguration::KisColorTransformationConfiguration(const QString & name, qint32 version, KisResourcesInterfaceSP resourcesInterface)
    : KisFilterConfiguration(name, version, resourcesInterface)
    , d(new Private())
{
}

KisColorTransformationConfiguration::KisColorTransformationConfiguration(const KisColorTransformationConfiguration &rhs)
    : KisFilterConfiguration(rhs)
    , d(new Private())
{
}

KisColorTransformationConfiguration::~KisColorTransformationConfiguration()
{
    delete d;
}

KisFilterConfigurationSP KisColorTransformationConfiguration::clone() const
{
    return new KisColorTransformationConfiguration(*this);
}

/**
 * Invalidate the cache by default when setProperty is called. This forces
 * regenerating the color transforms also when a property of this object
 * changes, not only when the object is copied
 */
void KisColorTransformationConfiguration::setProperty(const QString &name, const QVariant &value)
{
    KisFilterConfiguration::setProperty(name, value);
    invalidateColorTransformationCache();
}

KoColorTransformation* KisColorTransformationConfiguration::colorTransformation(const KoColorSpace *cs, const KisColorTransformationFilter *filter) const
{
    QMutexLocker locker(&d->mutex);
    KoColorTransformation *transformation = d->colorTransformation.value(QThread::currentThread(), 0);
    if (!transformation) {
        KisFilterConfigurationSP config(clone().data());
        transformation = filter->createTransformation(cs, config);
        d->colorTransformation.insert(QThread::currentThread(), transformation);
    }
    locker.unlock();
    return transformation;
}

KisGpuColorTransformSP KisColorTransformationConfiguration::gpuColorTransform(const KoColorSpace *cs, const KisColorTransformationFilter *filter) const
{
    QMutexLocker locker(&d->mutex);
    auto it = d->gpuColorTransforms.constFind(cs);
    if (it != d->gpuColorTransforms.constEnd()) {
        return it.value();
    }
    KisFilterConfigurationSP config(clone().data());
    KisGpuColorTransformSP transform = filter->createGpuColorTransform(cs, config);
    d->gpuColorTransforms.insert(cs, transform);
    return transform;
}

void KisColorTransformationConfiguration::invalidateColorTransformationCache()
{
    d->destroyCache();
}
