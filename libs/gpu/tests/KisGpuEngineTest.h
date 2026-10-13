/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISGPUENGINETEST_H
#define KISGPUENGINETEST_H

#include <QObject>
#include <QString>

#include <memory>

class KisGpuContext;

class KisGpuEngineTest : public QObject
{
    Q_OBJECT
public:
    KisGpuEngineTest();
    ~KisGpuEngineTest() override;

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testDeviceInfo();
    void testSoftwareDevicesRefused();
    void testTilePoolAllocation();
    void testUploadReadbackRoundTrip();
    void testEarlyMainFinish();
    void testConcurrentSubmissionTiming();
    void testCompositeOverMatchesKoCompositeOp();
    void testCompositeOverF16();
    void testSharedComputePipelines();
    void testPreciseContraction_data();
    void testPreciseContraction();
    void testBlendProbe();
    void benchmarkCompositeStack();

private:
    std::unique_ptr<KisGpuContext> m_context;
    QString m_unavailableReason;
};

#endif // KISGPUENGINETEST_H
