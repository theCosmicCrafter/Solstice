#include "VisionMLPlugin.h"
#include "VisionML.h"
#include "filters/BackgroundRemovalFilter.h"
#include "inpaint/InpaintTool.h"
#include "segmentation/SelectSegmentFromPointTool.h"
#include "segmentation/SelectSegmentFromRectTool.h"
#include "smartselect/SmartMattingTool.h"

#include <KoToolRegistry.h>
#include <QDebug>
#include <filter/kis_filter_registry.h>
#include <kis_debug.h>
#include <kis_global.h>
#include <kis_types.h>
#include <kpluginfactory.h>

K_PLUGIN_FACTORY_WITH_JSON(VisionMLPluginFactory, "kritavisionml.json", registerPlugin<VisionMLPlugin>();)

VisionMLPlugin::VisionMLPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    if (QSharedPointer<VisionModels> shared = VisionModels::create()) {
        auto addTool = [](KoToolFactoryBase *toolFactory) {
            qDebug() << "[VisionMLPlugin] Registering tool factory:" << toolFactory->id();
            KoToolRegistry::instance()->add(toolFactory);
        };

        addTool(new SelectSegmentFromPointToolFactory(shared));
        addTool(new SelectSegmentFromRectToolFactory(shared));
        addTool(new SmartMattingToolFactory(shared));
        addTool(new InpaintToolFactory(shared));

        KisFilterRegistry::instance()->add(new BackgroundRemovalFilter(shared));
    } else {
        qWarning() << "[VisionMLPlugin] Failed to create VisionModels instance. Tools will not be available.";
    }
}

VisionMLPlugin::~VisionMLPlugin()
{
}

#include "VisionMLPlugin.moc"
