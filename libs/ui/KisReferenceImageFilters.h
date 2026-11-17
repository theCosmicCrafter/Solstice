/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <kritaui_export.h>
class QMenu;
class KoCanvasBase;
class KisReferenceImage;
class KisViewManager;
KRITAUI_EXPORT void populateReferenceImageFilterMenu(QMenu *, KisReferenceImage *, KoCanvasBase *, KisViewManager *);
