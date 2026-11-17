/*
 *  SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VIEW_RES_VECTOR_COMPOSITOR_TEST_H
#define KIS_VIEW_RES_VECTOR_COMPOSITOR_TEST_H

#include <testui.h>

class KisViewResVectorCompositorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testUnblendSourceOver();
    void testCollectUnits();
    void testComputeJobs();
    void testBackdropAndRecomposite();
};

#endif // KIS_VIEW_RES_VECTOR_COMPOSITOR_TEST_H
