/*
 *  SPDX-FileCopyrightText: 2012 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_layer_composition.h"
#include "kis_node_visitor.h"
#include "kis_group_layer.h"
#include "kis_adjustment_layer.h"
#include "kis_external_layer_iface.h"
#include "kis_paint_layer.h"
#include "generator/kis_generator_layer.h"
#include "kis_clone_layer.h"
#include "kis_filter_mask.h"
#include "kis_transform_mask.h"
#include "kis_transparency_mask.h"
#include "kis_selection_mask.h"
#include "lazybrush/kis_colorize_mask.h"
#include "kis_layer_utils.h"
#include "kis_node_query_path.h"

#include <QDomDocument>
#include <QMutexLocker>
#include "commands/kis_node_property_list_command.h"
#include "kis_layer_properties_icons.h"
#include "kis_command_utils.h"

class KisCompositionVisitor : public KisNodeVisitor {
public:
    enum Mode {
        STORE,
        APPLY
    };
    
    KisCompositionVisitor(KisLayerComposition* layerComposition, Mode mode)
        : m_layerComposition(layerComposition)
        , m_mode(mode)
    {        
    }

    bool visit(KisNode* node) override { return process(node); }
    bool visit(KisGroupLayer* layer) override
    { 
        if(layer == layer->image()->rootLayer()) {
            return visitAll(layer);
        }
        return process(layer);
    }
    bool visit(KisAdjustmentLayer* layer) override { return process(layer); }
    bool visit(KisPaintLayer* layer) override { return process(layer); }
    bool visit(KisExternalLayer* layer) override { return process(layer); }
    bool visit(KisGeneratorLayer* layer) override { return process(layer); }
    bool visit(KisCloneLayer* layer) override { return process(layer); }
    bool visit(KisFilterMask* mask) override { return process(mask); }
    bool visit(KisTransformMask* mask) override { return process(mask); }
    bool visit(KisTransparencyMask* mask) override { return process(mask); }
    bool visit(KisSelectionMask* mask) override { return process(mask); }
    bool visit(KisColorizeMask* mask) override { return process(mask); }

    bool process(KisNode* node) {
        if (node->isFakeNode()) {
            dbgKrita << "Compositions: Skipping over Fake Node" << node->uuid() << node->name();
            return true;
        }

        bool result = visitAll(node);

        if(m_mode == STORE) {
            m_layerComposition->m_visibilityMap[node->uuid()] = node->visible();
            m_layerComposition->m_collapsedMap[node->uuid()] = node->collapsed();
        } else {
            const bool newState = m_layerComposition->m_visibilityMap.value(node->uuid(), false);
            if(node->visible() != newState) {
                node->setVisible(newState);
                node->setDirty();
            }
            if(m_layerComposition->m_collapsedMap.contains(node->uuid())) {
                node->setCollapsed(m_layerComposition->m_collapsedMap[node->uuid()]);
            }
        }
        
        return result;
    }
private:
    KisLayerComposition* m_layerComposition;
    Mode m_mode;
};

KisLayerComposition::KisLayerComposition(KisImageWSP image, const QString& name)
    : m_image(image)
    , m_name(name)
    , m_uuid(QUuid::createUuid())
    , m_exportEnabled(true)
{

}

KisLayerComposition::~KisLayerComposition()
{

}

KisLayerComposition::KisLayerComposition(const KisLayerComposition &rhs, KisImageWSP otherImage)
    : m_image(otherImage ? otherImage : rhs.m_image)
{
    QMutexLocker locker(&rhs.m_mutex);
    m_name = rhs.m_name;
    m_uuid = rhs.m_uuid;
    m_exportEnabled = rhs.m_exportEnabled;
    {
        auto it = rhs.m_visibilityMap.constBegin();
        for (; it != rhs.m_visibilityMap.constEnd(); ++it) {
            QUuid nodeUuid = it.key();
            KisNodeSP node = KisLayerUtils::findNodeByUuid(rhs.m_image->root(), nodeUuid);
            if (node) {
                KisNodeQueryPath path = KisNodeQueryPath::absolutePath(node);
                KisNodeSP newNode = path.queryUniqueNode(m_image);
                KIS_ASSERT_RECOVER(newNode) { continue; }

                m_visibilityMap.insert(newNode->uuid(), it.value());
            }
        }
    }

    {
        auto it = rhs.m_collapsedMap.constBegin();
        for (; it != rhs.m_collapsedMap.constEnd(); ++it) {
            QUuid nodeUuid = it.key();
            KisNodeSP node = KisLayerUtils::findNodeByUuid(rhs.m_image->root(), nodeUuid);
            if (node) {
                KisNodeQueryPath path = KisNodeQueryPath::absolutePath(node);
                KisNodeSP newNode = path.queryUniqueNode(m_image);
                KIS_ASSERT_RECOVER(newNode) { continue; }

                m_collapsedMap.insert(newNode->uuid(), it.value());
            }
        }
    }
}

void KisLayerComposition::setName(const QString& name)
{
    QMutexLocker locker(&m_mutex);
    m_name = name;
}

QString KisLayerComposition::name()
{
    QMutexLocker locker(&m_mutex);
    return m_name;
}

QUuid KisLayerComposition::uuid() const
{
    QMutexLocker locker(&m_mutex);
    return m_uuid;
}

void KisLayerComposition::setUuid(const QUuid &uuid)
{
    QMutexLocker locker(&m_mutex);
    m_uuid = uuid;
}

void KisLayerComposition::store()
{
    QMutexLocker locker(&m_mutex);
    if(m_image.isNull()) {
        return;
    }
    KisCompositionVisitor visitor(this, KisCompositionVisitor::STORE);
    m_image->rootLayer()->accept(visitor);
}

void KisLayerComposition::apply()
{
    QMutexLocker locker(&m_mutex);
    if (m_image.isNull()) {
        return;
    }
    KisCompositionVisitor visitor(this, KisCompositionVisitor::APPLY);
    m_image->rootLayer()->accept(visitor);
}

void KisLayerComposition::setExportEnabled ( bool enabled )
{
    QMutexLocker locker(&m_mutex);
    m_exportEnabled = enabled;
}

bool KisLayerComposition::isExportEnabled()
{
    QMutexLocker locker(&m_mutex);
    return m_exportEnabled;
}

void KisLayerComposition::setVisible(QUuid id, bool visible)
{
    QMutexLocker locker(&m_mutex);
    m_visibilityMap[id] = visible;
}

void KisLayerComposition::setCollapsed ( QUuid id, bool collapsed )
{
    QMutexLocker locker(&m_mutex);
    m_collapsedMap[id] = collapsed;
}

void KisLayerComposition::save(QDomDocument& doc, QDomElement& element)
{
    QMutexLocker locker(&m_mutex);
    QDomElement compositionElement = doc.createElement("composition");
    compositionElement.setAttribute("name", m_name);
    compositionElement.setAttribute("exportEnabled", m_exportEnabled);
    // Solstice: an attribute, which older loaders and Krita ignore.
    compositionElement.setAttribute("uuid", m_uuid.toString());
    QMapIterator<QUuid, bool> iter(m_visibilityMap);
    while (iter.hasNext()) {
        iter.next();
        QDomElement valueElement = doc.createElement("value");
        dbgKrita << "uuid" << iter.key().toString() << "visible" <<  iter.value();
        valueElement.setAttribute("uuid", iter.key().toString());
        valueElement.setAttribute("visible", iter.value());
        dbgKrita << "contains" << m_collapsedMap.contains(iter.key());
        if (m_collapsedMap.contains(iter.key())) {
            dbgKrita << "collapsed :" << m_collapsedMap[iter.key()];
            valueElement.setAttribute("collapsed", m_collapsedMap[iter.key()]);
        }
        compositionElement.appendChild(valueElement);
    }
    element.appendChild(compositionElement);
}

QMap<QUuid, bool> KisLayerComposition::visibilityMap() const
{
    QMutexLocker locker(&m_mutex);
    return m_visibilityMap;
}

void KisLayerComposition::recordAddedNode(KisNodeSP node, bool visible)
{
    if (node->isFakeNode())
        return;
    QMutexLocker locker(&m_mutex);
    if (!m_visibilityMap.contains(node->uuid())) {
        m_visibilityMap.insert(node->uuid(), visible);
        m_collapsedMap.insert(node->uuid(), node->collapsed());
    }
}

bool KisLayerComposition::matchesCurrentVisibility(bool preserveMissing) const
{
    if (!m_image)
        return false;
    const auto visibility = visibilityMap();
    bool matches = true;
    KisLayerUtils::recursiveApplyNodes(m_image->root(), [&](KisNodeSP node) {
        if (node == m_image->root() || node->isFakeNode())
            return;
        if (preserveMissing && !visibility.contains(node->uuid()))
            return;
        matches &= node->visible() == visibility.value(node->uuid(), false);
    });
    return matches;
}

KUndo2Command *KisLayerComposition::createApplyCommand(bool preserveMissing)
{
    if (!m_image)
        return nullptr;
    QMap<QUuid, bool> visibility;
    QMap<QUuid, bool> collapsed;
    {
        QMutexLocker locker(&m_mutex);
        visibility = m_visibilityMap;
        collapsed = m_collapsedMap;
    }
    auto *command = new KisCommandUtils::CompositeCommand();
    command->setText(kundo2_i18n("Apply Layer Comp"));
    bool changed = false;
    KisLayerUtils::recursiveApplyNodes(m_image->root(), [&](KisNodeSP node) {
        if (node == m_image->root() || node->isFakeNode())
            return;
        if (preserveMissing && !visibility.contains(node->uuid()))
            return;
        const bool target = visibility.value(node->uuid(), false);
        if (node->visible() != target) {
            auto properties = node->sectionModelProperties();
            KisLayerPropertiesIcons::setNodeProperty(&properties, KisLayerPropertiesIcons::visible, target);
            command->addCommand(new KisNodePropertyListCommand(node, properties));
            changed = true;
        }
        if (collapsed.contains(node->uuid()))
            node->setCollapsed(collapsed.value(node->uuid()));
    });
    if (!changed) {
        delete command;
        return nullptr;
    }
    return command;
}
