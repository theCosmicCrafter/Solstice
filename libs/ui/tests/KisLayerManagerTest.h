/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISLAYERMANAGERTEST_H
#define KISLAYERMANAGERTEST_H

#include <QObject>

class KisLayerManagerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testRectToPointsInPt();
    void testDonutKeepsHole();
    void testEmptyLayerAddsNothing();
    void testThreshold50Percent();
};

#endif // KISLAYERMANAGERTEST_H
