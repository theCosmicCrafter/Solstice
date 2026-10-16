/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kis_svg_export.h"

#include <functional>

#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include <KisDocument.h>
#include <KisExportCheckRegistry.h>
#include <KisImportExportErrorCode.h>
#include <KisImportExportManager.h>
#include <KoShapeLayer.h>
#include <SvgWriter.h>
#include <kis_assert.h>
#include <kis_image.h>

#include "kis_shape_layer.h"

K_PLUGIN_FACTORY_WITH_JSON(KisSVGExportFactory, "krita_svg_export.json", registerPlugin<KisSVGExport>();)

KisSVGExport::KisSVGExport(QObject *parent, const QVariantList &)
    : KisImportExportFilter(parent)
{
}

KisSVGExport::~KisSVGExport()
{
}

KisImportExportErrorCode
KisSVGExport::convert(KisDocument *document, QIODevice *io, KisPropertiesConfigurationSP configuration)
{
    Q_UNUSED(configuration);

    KisImageSP image = document->savingImage();
    KIS_ASSERT_RECOVER_RETURN_VALUE(image, ImportExportCodes::InternalError);

    /**
     * Only the vector layers end up in the file, each as a <g> group, in
     * stacking order. Krita stores siblings top-first (firstChild() is the
     * topmost node, nextSibling() walks down), so iterate lastChild ->
     * prevSibling to emit bottom layers first: SVG paints later elements
     * on top. Raster layers have no vector representation; shapes the
     * writer has no native element for are embedded as images (SvgWriter).
     */
    QList<KoShapeLayer *> layers;
    const std::function<void(KisNodeSP)> collect = [&layers, &collect](KisNodeSP node) {
        for (KisNodeSP child = node->lastChild(); child; child = child->prevSibling()) {
            collect(child);
        }
        KisShapeLayer *layer = dynamic_cast<KisShapeLayer *>(node.data());
        if (layer && node->visible(true)) {
            layers << layer;
        }
    };
    collect(KisNodeSP(image->root()));

    if (layers.isEmpty()) {
        return KisImportExportErrorCode(i18n("The document contains no vector layers."));
    }

    const QSizeF sizeInPx = image->bounds().size();
    const QSizeF sizeInPt(sizeInPx.width() / image->xRes(), sizeInPx.height() / image->yRes());

    SvgWriter writer(layers);
    if (!writer.save(*io, sizeInPt)) {
        return ImportExportCodes::ErrorWhileWriting;
    }

    return ImportExportCodes::OK;
}

void KisSVGExport::initializeCapabilities()
{
    // Vector layers are written as <g> groups; declaring multilayer
    // support keeps Advanced Export from flattening the image, which
    // would destroy the vectors
    addCapability(KisExportCheckRegistry::instance()->get("MultiLayerCheck")->create(KisExportCheckBase::SUPPORTED));
    addCapability(
        KisExportCheckRegistry::instance()->get("NodeTypeCheck/KisShapeLayer")->create(KisExportCheckBase::SUPPORTED));

    // Layer opacity and blending are not written per group
    addCapability(
        KisExportCheckRegistry::instance()->get("LayerOpacityCheck")->create(KisExportCheckBase::UNSUPPORTED));
}

#include "kis_svg_export.moc"
