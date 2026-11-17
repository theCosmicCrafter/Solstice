/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisChromaticAberrationFilter.h"
#include "KisColorFilter.h"
#include "KisColorReferenceFilter.h"
#include "KisCrystalFilter.h"
#include "KisDropShadowFilter.h"
#include "KisFilmFilter.h"
#include "KisFilmGrainFilter.h"
#include "KisGlowFilter.h"
#include "KisLineArtFilter.h"
#include "KisPencilFilter.h"
#include "KisRemoveDustFilter.h"
#include "KisSharpnessFilter.h"

#include <QObject>
#include <filter/kis_filter_registry.h>
#include <kpluginfactory.h>

class SolsticeEffectsPlugin : public QObject
{
    Q_OBJECT
public:
    SolsticeEffectsPlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
        KisFilterRegistry::instance()->add(new KisFilmGrainFilter());
        KisFilterRegistry::instance()->add(new KisChromaticAberrationFilter());
        KisFilterRegistry::instance()->add(new KisFilmFilter());
        KisFilterRegistry::instance()->add(new KisCrystalFilter());
        KisFilterRegistry::instance()->add(new KisPencilFilter());
        KisFilterRegistry::instance()->add(new KisSharpnessFilter());
        KisFilterRegistry::instance()->add(new KisColorFilter());
        KisFilterRegistry::instance()->add(new KisGlowFilter());
        KisFilterRegistry::instance()->add(new KisDropShadowFilter());
        KisFilterRegistry::instance()->add(new KisLineArtFilter());
        KisFilterRegistry::instance()->add(new KisColorReferenceFilter());
        KisFilterRegistry::instance()->add(new KisRemoveDustFilter());
    }
};

K_PLUGIN_FACTORY_WITH_JSON(SolsticeEffectsFactory,
                           "kritasolsticeeffectsfilter.json",
                           registerPlugin<SolsticeEffectsPlugin>();)

#include "SolsticeEffectsPlugin.moc"
