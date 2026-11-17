/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestKoSpectralMixing.h"

#include <testpigment.h>

#include <kis_global.h>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>
#include <KoSpectralMixing.h>

void TestKoSpectralMixing::testReflectanceRoundTrip()
{
    // linear sRGB -> reflectance -> linear sRGB should be nearly lossless
    // for in-gamut colors (the base spectra were solved to reproduce them).
    const float colors[][3] = {
        {0.0f, 0.0f, 1.0f}, // blue
        {1.0f, 1.0f, 0.0f}, // yellow
        {1.0f, 0.0f, 0.0f}, // red
        {0.0f, 1.0f, 0.0f}, // green
        {0.5f, 0.5f, 0.5f}, // mid gray
        {0.2f, 0.4f, 0.8f}, // light blue
        {1.0f, 1.0f, 1.0f}, // white
    };

    float R[KoSpectralMixing::SampleCount];
    float rgb[3];

    for (const auto &c : colors) {
        KoSpectralMixing::lrgbToReflectance(c[0], c[1], c[2], R);
        KoSpectralMixing::reflectanceToLrgb(R, rgb);
        for (int ch = 0; ch < 3; ++ch) {
            QVERIFY2(qAbs(rgb[ch] - c[ch]) < 0.03,
                     qPrintable(QString("round-trip drift %1 vs %2").arg(rgb[ch]).arg(c[ch])));
        }
    }
}

void TestKoSpectralMixing::testBlueYellowGivesGreen()
{
    // spectral.js reference value: 50/50 mix of (0,0,1) and (1,1,0)
    // gives roughly (0.22, 0.56, 0.33) linear — a green.
    const float blue[3] = {0.0f, 0.0f, 1.0f};
    const float yellow[3] = {1.0f, 1.0f, 0.0f};
    float out[3];

    KoSpectralMixing::mixLrgb(blue, yellow, 0.5f, 0.5f, out);

    QVERIFY2(out[1] > out[0] && out[1] > out[2],
             qPrintable(QString("blue+yellow is not green: %1 %2 %3").arg(out[0]).arg(out[1]).arg(out[2])));

    // Reference values from the Python port (tools/spectral_proto.py):
    // 0.5 spectral mix -> sRGB (57,143,84), i.e. linear
    // approx (0.046, 0.276, 0.090); keep a generous band.
    const float encG = KoSpectralMixing::compandSrgb(out[1]);
    QVERIFY(encG > 0.4f && encG < 0.7f);
    QVERIFY(out[0] < out[2]);
}

void TestKoSpectralMixing::testTintingWithWhiteAndBlack()
{
    const float white[3] = {1.0f, 1.0f, 1.0f};
    const float black[3] = {0.0f, 0.0f, 0.0f};
    const float blue[3] = {0.0f, 0.0f, 0.8f};
    float out[3];

    // white + color: lightens but keeps the hue dominant
    KoSpectralMixing::mixLrgb(blue, white, 0.5f, 0.5f, out);
    QVERIFY2(out[2] > out[0] && out[2] > out[1],
             qPrintable(QString("blue+white lost blue: %1 %2 %3").arg(out[0]).arg(out[1]).arg(out[2])));
    QVERIFY(out[2] > 0.6f); // clearly lightened

    // black + color: darkens
    KoSpectralMixing::mixLrgb(blue, black, 0.5f, 0.5f, out);
    QVERIFY2(out[2] < 0.5f,
             qPrintable(QString("blue+black not darkened: %1 %2 %3").arg(out[0]).arg(out[1]).arg(out[2])));
    QVERIFY(out[2] >= out[0] && out[2] >= out[1]); // still blue-ish
}

void TestKoSpectralMixing::testCompositeOpBlueOverYellow()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs->compositeOp(COMPOSITE_PIGMENT);
    QVERIFY(op);

    KoColor dst(QColor(255, 255, 0), cs); // yellow canvas
    KoColor src(QColor(0, 0, 255), cs); // opaque blue paint

    op->composite(dst.data(), 4, src.data(), 4, nullptr, 0, 1, 1, OPACITY_OPAQUE_F);

    const QColor result = dst.toQColor();

    // Blue + yellow under Kubelka-Munk is green; normal compositing
    // would leave (0,0,255) blue. Assert a clear green.
    QVERIFY2(result.green() > result.red() && result.green() > result.blue(),
             qPrintable(QString("pigment mix is not green: %1").arg(result.name())));
    QVERIFY(result.alpha() == 255);
}

void TestKoSpectralMixing::testCompositeOpAlphaBehavior()
{
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs->compositeOp(COMPOSITE_PIGMENT);
    QVERIFY(op);

    // zero-opacity source must leave dst untouched
    {
        KoColor dst(QColor(255, 255, 0), cs);
        const QColor before = dst.toQColor();
        KoColor src(QColor(0, 0, 255), cs);

        op->composite(dst.data(), 4, src.data(), 4, nullptr, 0, 1, 1, 0.0f);
        QCOMPARE(dst.toQColor(), before);
    }

    // transparent destination gets the source color (same as Normal)
    {
        KoColor dst(Qt::transparent, cs);
        KoColor src(QColor(0, 0, 255), cs);

        op->composite(dst.data(), 4, src.data(), 4, nullptr, 0, 1, 1, OPACITY_OPAQUE_F);
        QCOMPARE(dst.toQColor(), QColor(0, 0, 255));
    }

    // small source share over dst should lean towards the destination
    {
        KoColor dst(QColor(255, 255, 0), cs);
        KoColor src(QColor(0, 0, 255), cs);

        op->composite(dst.data(), 4, src.data(), 4, nullptr, 0, 1, 1, 0.25f);
        const QColor result = dst.toQColor();
        QVERIFY2(result.red() > result.blue(),
                 qPrintable(QString("weak pigment share should lean yellow: %1").arg(result.name())));
    }
}

KISTEST_MAIN(TestKoSpectralMixing)
