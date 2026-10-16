/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VISIONML_MATTE_TEST_H_
#define VISIONML_MATTE_TEST_H_

#include <simpletest.h>

class VisionMLMatteTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testModulateMatteAlphaFull();
    void testModulateMatteAlphaZeros();
    void testModulateMatteAlphaAntialiasedMask();
    void testModulateMatteAlphaSemiTransparent();
    void testExpandRegionToMin();
    void testExpandRegionBoundaryConditions();
    void testClampRegionToMax();
};

#endif // VISIONML_MATTE_TEST_H_
