/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KIS_TOOL_REMOVE_DUST_FACTORY_H
#define KIS_TOOL_REMOVE_DUST_FACTORY_H

#include <KisToolPaintFactoryBase.h>
#include <KoIcon.h>
#include <klocalizedstring.h>

#include "KisToolRemoveDust.h"

class KisToolRemoveDustFactory : public KisToolPaintFactoryBase
{
public:
    KisToolRemoveDustFactory()
        : KisToolPaintFactoryBase("KisToolRemoveDust")
    {
        setToolTip(i18n("Remove Dust Tool"));
        setSection(ToolBoxSection::Fill);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
        setIconName(koIconNameCStr("krita_tool_remove_dust"));
        setPriority(16);
    }

    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new KisToolRemoveDust(canvas);
    }
};

#endif // KIS_TOOL_REMOVE_DUST_FACTORY_H
