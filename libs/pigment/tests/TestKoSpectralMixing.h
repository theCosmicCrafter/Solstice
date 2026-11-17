/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TESTKOSPECTRALMIXING_H
#define TESTKOSPECTRALMIXING_H

#include <QObject>

class TestKoSpectralMixing : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testReflectanceRoundTrip();
    void testBlueYellowGivesGreen();
    void testTintingWithWhiteAndBlack();
    void testCompositeOpBlueOverYellow();
    void testCompositeOpAlphaBehavior();
};

#endif
