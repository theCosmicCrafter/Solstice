/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2012 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2014 Mohit Goyal <mohit.bits2011@gmail.com>
 * SPDX-FileCopyrightText: 2021 Deif Lou <ginoba@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "kis_texture_option.h"

#include <QString>
#include <QCheckBox>
#include <QBuffer>
#include <QFormLayout>
#include <QPainter>
#include <QBoxLayout>

#include <klocalizedstring.h>

#include <KoPattern.h>
#include <KoAbstractGradient.h>
#include <KoResource.h>
#include <KoResourceServerProvider.h>
#include <kis_paint_device.h>
#include <kis_fill_painter.h>
#include <kis_painter.h>
#include <kis_iterator_ng.h>
#include <kis_fixed_paint_device.h>
#include "KoMixColorsOp.h"
#include <strokes/KisMaskingBrushCompositeOpBase.h>
#include <strokes/KisMaskingBrushCompositeOpFactory.h>
#include <kis_random_accessor_ng.h>
#include <KoCompositeOpRegistry.h>

#include <KoCanvasResourcesIds.h>
#include <KoCanvasResourcesInterface.h>
#include <KoResourceLoadResult.h>
#include <kis_paint_information.h>

// GPU engine (Solstice): textured dab descriptions
#include <KisProceduralCircleDab.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpaceMaths.h>
#include <strokes/KisMaskingBrushCompositeOp.h>

/**********************************************************************/
/*       KisTextureOption                                             */
/**********************************************************************/


KisTextureOption::KisTextureOption(const KisPropertiesConfiguration *setting,
                                   KisResourcesInterfaceSP resourcesInterface,
                                   KoCanvasResourcesInterfaceSP canvasResourcesInterface,
                                   int levelOfDetail,
                                   KisBrushTextureFlags flags)
    : m_gradient(0)
    , m_levelOfDetail(levelOfDetail)
    , m_strengthOption(setting)
    , m_flags(flags)
{
    fillProperties(setting, resourcesInterface, canvasResourcesInterface);
}

void KisTextureOption::fillProperties(const KisPropertiesConfiguration *setting, KisResourcesInterfaceSP resourcesInterface, KoCanvasResourcesInterfaceSP canvasResourcesInterface)
{
    KisTextureOptionData data;
    data.read(setting);

    if (data.textureData.isNull()) {
        m_enabled = false;
        return;
    }

    m_texturingMode = data.texturingMode;
    m_useSoftTexturing = data.useSoftTexturing;

    if (!(m_flags & SupportsLightnessMode) && (m_texturingMode == KisTextureOptionData::LIGHTNESS)) {
        m_texturingMode = KisTextureOptionData::SUBTRACT;
    }

    if (!(m_flags & SupportsGradientMode) && (m_texturingMode == KisTextureOptionData::GRADIENT)) {
        m_texturingMode = KisTextureOptionData::SUBTRACT;
    }

    const bool preserveAlpha = m_texturingMode == KisTextureOptionData::LIGHTNESS || m_texturingMode == KisTextureOptionData::GRADIENT;

    QString effectiveCompositeOp = COMPOSITE_OVER;
    bool additionalInvert = false;

    if (canvasResourcesInterface && data.autoInvertOnErase) {
        effectiveCompositeOp = canvasResourcesInterface->resource(KoCanvasResource::CurrentEffectiveCompositeOp).toString();
        KIS_SAFE_ASSERT_RECOVER (!effectiveCompositeOp.isEmpty()) {
            effectiveCompositeOp = COMPOSITE_OVER;
        }
        additionalInvert = effectiveCompositeOp == COMPOSITE_ERASE;
    }

    m_maskInfo = toQShared(new KisTextureMaskInfo(m_levelOfDetail, preserveAlpha));
    if (!m_maskInfo->fillProperties(setting, resourcesInterface, additionalInvert)) {
        warnKrita << "WARNING: Couldn't load the pattern for a stroke (KisTextureProperties)";
        m_enabled = false;
        return;
    }

    m_maskInfo = KisTextureMaskInfoCache::instance()->fetchCachedTextureInfo(m_maskInfo);

    m_enabled = data.isEnabled;
    m_offsetX = data.offsetX;
    m_offsetY = data.offsetY;
    m_isRandomOffsetX = data.isRandomOffsetX;
    m_isRandomOffsetY = data.isRandomOffsetY;

    if (m_texturingMode == KisTextureOptionData::GRADIENT && canvasResourcesInterface) {
        KoAbstractGradientSP gradient = canvasResourcesInterface->resource(KoCanvasResource::CurrentGradient).value<KoAbstractGradientSP>()->cloneAndBakeVariableColors(canvasResourcesInterface);
        if (gradient) {
            m_gradient = gradient;
            m_cachedGradient.setGradient(gradient, 256);
        }
    }
}

int KisTextureOption::effectiveOffsetX(const KisPaintInformation &info) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_maskInfo, 0);
    return m_isRandomOffsetX ?
        info.perStrokeRandomSource()->generate("texture_offset_x", 0, m_maskInfo->maskBounds().width()) :
        m_offsetX;
}

int KisTextureOption::effectiveOffsetY(const KisPaintInformation &info) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_maskInfo, 0);
    return m_isRandomOffsetY ?
        info.perStrokeRandomSource()->generate("texture_offset_y", 0, m_maskInfo->maskBounds().height()) :
        m_offsetY;
}

QList<KoResourceLoadResult> KisTextureOption::prepareEmbeddedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface)
{
    /**
     * We cannot use m_enabled here because it is not initialized at this stage.
     * fillProperties() is not necessary for this call, because it is extremely slow.
     */

    QList<KoResourceLoadResult> patterns;

    KisTextureOptionData data;
    data.read(setting.data());

    if (data.isEnabled && !data.textureData.patternBase64.isEmpty()) {
        patterns << data.textureData.loadLinkedPattern(resourcesInterface);
    }

    return patterns;
}

QList<KoResourceLoadResult> KisTextureOption::prepareLinkedResources(const KisPropertiesConfigurationSP setting, KisResourcesInterfaceSP resourcesInterface)
{
    /**
     * We cannot use m_enabled here because it is not initialized at this stage.
     * fillProperties() is not necessary for this call, because it is extremely slow.
     */

    QList<KoResourceLoadResult> patterns;

    KisTextureOptionData data;
    data.read(setting.data());

    if (data.isEnabled && data.textureData.patternBase64.isEmpty()) {
        patterns << data.textureData.loadLinkedPattern(resourcesInterface);
    }

    return patterns;
}

bool KisTextureOption::applyingGradient() const
{
    return m_texturingMode == KisTextureOptionData::GRADIENT;
}

bool KisTextureOption::applyingGradient(const KisPropertiesConfiguration *settings)
{
    KisTextureOptionData data;
    data.read(settings);

    return data.texturingMode == KisTextureOptionData::GRADIENT;
}

bool KisTextureOption::requiresEffectiveCompositeOp(const KisPropertiesConfiguration *settings)
{
    KisTextureOptionData data;
    data.read(settings);

    return data.isEnabled && data.autoInvertOnErase;
}


void KisTextureOption::applyLightness(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info) {
    if (!m_enabled) return;
    if (!m_maskInfo->isValid()) return;

    KisPaintDeviceSP mask = m_maskInfo->mask();

    const QRect rect = dab->bounds();
    const QRect maskBounds = m_maskInfo->maskBounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->rgb8(), m_cachedPaintDevice);
    KisPaintDeviceSP fillMaskDevice = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillMaskPainter(fillMaskDevice);
    fillMaskPainter.setCompositeOpId(COMPOSITE_COPY);
    fillMaskPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillMaskPainter.end();

    qreal pressure = m_strengthOption.apply(info);
    quint8* dabData = dab->data();

    KisSequentialConstIterator it(fillMaskDevice, QRect(x, y, rect.width(), rect.height()));
    while (it.nextPixel()) {
        const QRgb *maskQRgb = reinterpret_cast<const QRgb*>(it.oldRawData());
        dab->colorSpace()->fillGrayBrushWithColorAndLightnessWithStrength(dabData, maskQRgb, dabData, pressure, 1);
        dabData += dab->pixelSize();
    }
}

void KisTextureOption::applyGradient(KisFixedPaintDeviceSP dab, const QPoint& offset, const KisPaintInformation& info) {
    if (!m_enabled) return;
    if (!m_maskInfo->isValid()) return;

    KIS_SAFE_ASSERT_RECOVER_RETURN(m_gradient && m_gradient->valid());

    KisPaintDeviceSP mask = m_maskInfo->mask();
    const QRect maskBounds = m_maskInfo->maskBounds();
    QRect rect = dab->bounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->rgb8(), m_cachedPaintDevice);
    KisPaintDeviceSP fillDevice = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillPainter(fillDevice);
    fillPainter.setCompositeOpId(COMPOSITE_COPY);
    fillPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillPainter.end();

    qreal pressure = m_strengthOption.apply(info);
    quint8* dabData = dab->data();

    //for gradient textures...
    KoMixColorsOp* colorMix = dab->colorSpace()->mixColorsOp();
    qint16 colorWeights[2];
    colorWeights[0] = qRound(pressure * 255);
    colorWeights[1] = 255 - colorWeights[0];
    quint8* colors[2];
    m_cachedGradient.setColorSpace(dab->colorSpace()); //Change colorspace here so we don't have to convert each pixel drawn

    KisHLineIteratorSP iter = fillDevice->createHLineIteratorNG(x, y, rect.width());
    for (int row = 0; row < rect.height(); ++row) {
        for (int col = 0; col < rect.width(); ++col) {

            const QRgb* maskQRgb = reinterpret_cast<const QRgb*>(iter->oldRawData());
            qreal gradientvalue = qreal(qGray(*maskQRgb))/255.0;//qreal(*iter->oldRawData()) / 255.0;
            KoColor paintcolor;
            paintcolor.setColor(m_cachedGradient.cachedAt(gradientvalue), dab->colorSpace());
            qreal paintOpacity = paintcolor.opacityF() * (qreal(qAlpha(*maskQRgb)) / 255.0);
            paintcolor.setOpacity(qMin(paintOpacity, dab->colorSpace()->opacityF(dabData)));
            colors[0] = paintcolor.data();
            KoColor dabColor(dabData, dab->colorSpace());
            colors[1] = dabColor.data();
            colorMix->mixColors(colors, colorWeights, 2, dabData);

            iter->nextPixel();
            dabData += dab->pixelSize();
        }
        iter->nextRow();
    }
}

void KisTextureOption::apply(KisFixedPaintDeviceSP dab, const QPoint &offset, const KisPaintInformation & info)
{
    if (!m_enabled) return;
    if (!m_maskInfo->isValid()) return;

    if (m_texturingMode == KisTextureOptionData::LIGHTNESS) {
        applyLightness(dab, offset, info);
        return;
    }
    else if (m_texturingMode == KisTextureOptionData::GRADIENT && m_gradient) {
        applyGradient(dab, offset, info);
        return;
    }

    QRect rect = dab->bounds();
    KisPaintDeviceSP mask = m_maskInfo->mask();
    const QRect maskBounds = m_maskInfo->maskBounds();

    KisCachedPaintDevice::Guard g(mask, KoColorSpaceRegistry::instance()->alpha8(), m_cachedPaintDevice);
    KisPaintDeviceSP maskPatch = g.device();

    int x = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    int y = offset.y() % maskBounds.height() - effectiveOffsetY(info);

    const QRect maskPatchRect = QRect(x, y, rect.width(), rect.height());

    KisFillPainter fillPainter(maskPatch);
    fillPainter.setCompositeOpId(COMPOSITE_COPY);
    fillPainter.fillRect(kisGrowRect(maskPatchRect, 1), mask, maskBounds);
    fillPainter.end();

    // Compute final strength
    qreal strength = m_strengthOption.apply(info);

    const QString compositeOpId = alphaCompositeOpId(m_texturingMode);
    if (compositeOpId.isEmpty()) {
        return;
    }
    applyAlphaMask(dab, maskPatch, maskPatchRect, compositeOpId, strength, m_useSoftTexturing);
}

QString KisTextureOption::alphaCompositeOpId(KisTextureOptionData::TexturingMode mode)
{
    switch (mode) {
    case KisTextureOptionData::MULTIPLY:
        return COMPOSITE_MULT;
    case KisTextureOptionData::SUBTRACT:
        return COMPOSITE_SUBTRACT;
    case KisTextureOptionData::DARKEN:
        return COMPOSITE_DARKEN;
    case KisTextureOptionData::OVERLAY:
        return COMPOSITE_OVERLAY;
    case KisTextureOptionData::COLOR_DODGE:
        return COMPOSITE_DODGE;
    case KisTextureOptionData::COLOR_BURN:
        return COMPOSITE_BURN;
    case KisTextureOptionData::LINEAR_DODGE:
        return COMPOSITE_LINEAR_DODGE;
    case KisTextureOptionData::LINEAR_BURN:
        return COMPOSITE_LINEAR_BURN;
    case KisTextureOptionData::HARD_MIX_PHOTOSHOP:
        return COMPOSITE_HARD_MIX_PHOTOSHOP;
    case KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP:
        return COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP;
    case KisTextureOptionData::HEIGHT:
        return QStringLiteral("height");
    case KisTextureOptionData::LINEAR_HEIGHT:
        return QStringLiteral("linear_height");
    case KisTextureOptionData::HEIGHT_PHOTOSHOP:
        return QStringLiteral("height_photoshop");
    case KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP:
        return QStringLiteral("linear_height_photoshop");
    default:
        return QString();
    }
}

void KisTextureOption::applyAlphaMask(KisFixedPaintDeviceSP dab,
                                      KisPaintDeviceSP maskPatch,
                                      const QRect &maskPatchRect,
                                      const QString &compositeOpId,
                                      qreal strength,
                                      bool useSoftTexturing)
{
    // Select mask compositing op
    KoChannelInfo::enumChannelValueType alphaChannelType = KoChannelInfo::UINT8;
    int alphaChannelOffset = -1;

    const QList<KoChannelInfo *> channels = dab->colorSpace()->channels();
    for (quint32 i = 0; i < dab->pixelSize(); i++) {
        if (channels[i]->channelType() == KoChannelInfo::ALPHA) {
            // TODO: check correctness for 16bits!
            alphaChannelOffset = channels[i]->pos()/* * channels[i]->size()*/;
            alphaChannelType = channels[i]->channelValueType();
            break;
        }
    }

    KIS_SAFE_ASSERT_RECOVER (alphaChannelOffset >= 0) {
        alphaChannelOffset = 0;
    }

    QScopedPointer<KisMaskingBrushCompositeOpBase> compositeOp;
    compositeOp.reset(KisMaskingBrushCompositeOpFactory::createForAlphaSrc(compositeOpId,
                                                                           alphaChannelType,
                                                                           dab->pixelSize(),
                                                                           alphaChannelOffset,
                                                                           strength,
                                                                           useSoftTexturing));

    // Apply the mask to the dab
    {
        quint8 *dabIt = nullptr;
        KisRandomConstAccessorSP maskPatchIt = maskPatch->createRandomConstAccessorNG();

        qint32 dabY = dab->bounds().y();
        qint32 maskPatchY = maskPatchRect.y();
        qint32 rowsRemaining = dab->bounds().height();
        const qint32 dabRowStride = dab->bounds().width() * dab->pixelSize();

        while (rowsRemaining > 0) {
            qint32 dabX = dab->bounds().x();
            qint32 maskPatchX = maskPatchRect.x();
            const qint32 numContiguousMaskPatchRows = maskPatchIt->numContiguousRows(maskPatchY);
            const qint32 rows = std::min(rowsRemaining, numContiguousMaskPatchRows);
            qint32 columnsRemaining = dab->bounds().width();

            while (columnsRemaining > 0) {
                const qint32 numContiguousMaskPatchColumns = maskPatchIt->numContiguousColumns(maskPatchX);
                const qint32 columns = std::min(columnsRemaining, numContiguousMaskPatchColumns);

                const qint32 maskPatchRowStride = maskPatchIt->rowStride(maskPatchX, maskPatchY);

                dabIt = dab->data() + (dabY * dab->bounds().width() + dabX) * dab->pixelSize();
                maskPatchIt->moveTo(maskPatchX, maskPatchY);

                compositeOp->composite(maskPatchIt->rawDataConst(), maskPatchRowStride,
                                       dabIt, dabRowStride,
                                       columns, rows);

                dabX += columns;
                maskPatchX += columns;
                columnsRemaining -= columns;

            }

            dabY += rows;
            maskPatchY += rows;
            rowsRemaining -= rows;
        }
    }
}

/**
 * GPU engine (Solstice): applies a described texture on the CPU when a
 * pending dab is materialized (the GPU refused the batch): the same mask
 * patch and composite as KisTextureOption::apply().
 */
class KisTextureCpuApplier : public KisProceduralDabTexture::CpuApplier
{
public:
    KisTextureCpuApplier(KisTextureMaskInfoSP maskInfo,
                         QPoint patchOrigin,
                         QString compositeOpId,
                         qreal strength,
                         bool soft)
        : m_maskInfo(maskInfo)
        , m_patchOrigin(patchOrigin)
        , m_compositeOpId(compositeOpId)
        , m_strength(strength)
        , m_soft(soft)
    {
    }

    void apply(KisFixedPaintDeviceSP dab) const override
    {
        const QRect rect = dab->bounds();
        const QRect maskPatchRect(m_patchOrigin, rect.size());
        KisPaintDeviceSP maskPatch = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
        KisFillPainter fillPainter(maskPatch);
        fillPainter.setCompositeOpId(COMPOSITE_COPY);
        fillPainter.fillRect(kisGrowRect(maskPatchRect, 1), m_maskInfo->mask(), m_maskInfo->maskBounds());
        fillPainter.end();
        KisTextureOption::applyAlphaMask(dab, maskPatch, maskPatchRect, m_compositeOpId, m_strength, m_soft);
    }

private:
    KisTextureMaskInfoSP m_maskInfo;
    QPoint m_patchOrigin;
    QString m_compositeOpId;
    qreal m_strength;
    bool m_soft;
};

QSharedPointer<const KisProceduralDabTexture> KisTextureOption::gpuDescription(const QPoint &offset,
                                                                               const KisPaintInformation &info,
                                                                               const KoColorSpace *dabColorSpace,
                                                                               quint32 brushFlips) const
{
    // RGBA F32, and RGBA F16 (phase 4.105)
    const bool halfDab =
        dabColorSpace && dabColorSpace->colorDepthId() == Float16BitsColorDepthID && dabColorSpace->pixelSize() == 8;
    if (!m_enabled || !m_maskInfo || !m_maskInfo->isValid() || !dabColorSpace
        || dabColorSpace->colorModelId() != RGBAColorModelID
        || !(halfDab
             || (dabColorSpace->colorDepthId() == Float32BitsColorDepthID && dabColorSpace->pixelSize() == 16))) {
        return {};
    }
    const QString compositeOpId = alphaCompositeOpId(m_texturingMode);
    const QSharedPointer<const QVector<quint8>> pattern = m_maskInfo->alphaBytes();
    const QRect maskBounds = m_maskInfo->maskBounds();
    if (compositeOpId.isEmpty() || !pattern || maskBounds.topLeft() != QPoint()
        || pattern->size() != maskBounds.width() * maskBounds.height()) {
        return {};
    }
    using namespace KisMaskingBrushCompositeDetail;
    QSharedPointer<KisProceduralDabTexture> texture(new KisProceduralDabTexture());
    texture->pattern = pattern;
    texture->width = maskBounds.width();
    texture->height = maskBounds.height();
    // The patch origin of apply()
    texture->originX = offset.x() % maskBounds.width() - effectiveOffsetX(info);
    texture->originY = offset.y() % maskBounds.height() - effectiveOffsetY(info);
    const qreal strength = m_strengthOption.apply(info);
    texture->soft = m_useSoftTexturing;
    texture->brushFlips = brushFlips & 3;
    // The composite function constants, computed as their constructors do
    // (KisMaskingBrushCompositeOp.h).
    switch (m_texturingMode) {
    case KisTextureOptionData::MULTIPLY:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_MULT;
        break;
    case KisTextureOptionData::SUBTRACT:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_SUBTRACT;
        break;
    case KisTextureOptionData::DARKEN:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_DARKEN;
        break;
    case KisTextureOptionData::OVERLAY:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_OVERLAY;
        break;
    case KisTextureOptionData::COLOR_DODGE:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_DODGE;
        break;
    case KisTextureOptionData::COLOR_BURN:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_BURN;
        break;
    case KisTextureOptionData::LINEAR_DODGE:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_LINEAR_DODGE;
        break;
    case KisTextureOptionData::LINEAR_BURN:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_LINEAR_BURN;
        break;
    case KisTextureOptionData::HARD_MIX_PHOTOSHOP:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_HARD_MIX_PHOTOSHOP;
        break;
    case KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP;
        break;
    case KisTextureOptionData::HEIGHT:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_HEIGHT;
        break;
    case KisTextureOptionData::LINEAR_HEIGHT:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_LINEAR_HEIGHT;
        break;
    case KisTextureOptionData::HEIGHT_PHOTOSHOP:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_HEIGHT_PHOTOSHOP;
        break;
    case KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP:
        texture->mode = KIS_MASKING_BRUSH_COMPOSITE_LINEAR_HEIGHT_PHOTOSHOP;
        break;
    default:
        return {};
    }
    const bool height = texture->mode == KIS_MASKING_BRUSH_COMPOSITE_HEIGHT
        || texture->mode == KIS_MASKING_BRUSH_COMPOSITE_LINEAR_HEIGHT;
    if (halfDab) {
        // the half instantiation; its weight is a double, exact in float
        const half halfStrength = StrengthCompositeFunctionBase<half>(height ? 0.99 * strength : strength).strength;
        texture->strength = float(halfStrength);
        texture->invertedStrength = float(Arithmetic::inv(halfStrength));
        texture->weight = float((m_useSoftTexturing ? double(9) : double(10)) * halfStrength);
    } else {
        texture->strength = StrengthCompositeFunctionBase<float>(height ? 0.99 * strength : strength).strength;
        texture->invertedStrength = Arithmetic::inv(texture->strength);
        texture->weight = (m_useSoftTexturing ? float(9) : float(10)) * texture->strength;
    }
    texture->cpu.reset(new KisTextureCpuApplier(m_maskInfo,
                                                QPoint(texture->originX, texture->originY),
                                                compositeOpId,
                                                strength,
                                                m_useSoftTexturing));
    return texture;
}
