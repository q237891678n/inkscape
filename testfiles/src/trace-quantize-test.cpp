// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for the color quantization used by Trace Bitmap's multiple scans.
 */
#include <algorithm>
#include <random>
#include <set>
#include <gtest/gtest.h>

#include "trace/filterset.h"
#include "trace/imagemap.h"
#include "trace/quantize.h"

using namespace Inkscape::Trace;

namespace {

RGB const RED = {0xb2, 0x00, 0x2d};
RGB const BLUE = {0x1d, 0x3f, 0x8f};
RGB const WHITE = {0xff, 0xff, 0xff};

bool same(RGB a, RGB b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// White, with a red block on the left and a blue one on the right.
RgbMap threeColors(int w = 60, int h = 40)
{
    RgbMap map(w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool inside = y >= 5 && y < h - 5;
            map.setPixel(x, y, inside && x >= 5 && x < 25 ? RED : inside && x >= 35 && x < 55 ? BLUE : WHITE);
        }
    }
    return map;
}

} // namespace

// Asking for more colors than the image has used to give pixels the index of an
// unfilled (black) palette entry, which was never traced: an empty result.
TEST(TraceQuantizeTest, FewerColorsThanAskedFor)
{
    auto map = threeColors();
    for (int ncolor : {3, 8, 64}) {
        auto imap = rgbMapQuantize(map, ncolor);
        ASSERT_EQ(imap.nrColors, 3) << ncolor;
        for (auto index : imap.pixels) {
            ASSERT_LT(index, 3u);
        }
        // Every pixel keeps its exact color.
        for (int y = 0; y < map.height; y++) {
            for (int x = 0; x < map.width; x++) {
                ASSERT_TRUE(same(imap.getPixelValue(x, y), map.getPixel(x, y))) << x << "," << y;
            }
        }
    }
}

// The palette is the exact (rounded) mean of its pixels: no color darkened by a step.
TEST(TraceQuantizeTest, ExactColors)
{
    auto imap = rgbMapQuantize(threeColors(), 3);
    std::set<unsigned> colors;
    for (int i = 0; i < imap.nrColors; i++) {
        auto c = imap.clut[i];
        colors.insert((c.r << 16) | (c.g << 8) | c.b);
    }
    EXPECT_EQ(colors, (std::set<unsigned>{0xb2002d, 0x1d3f8f, 0xffffff}));
}

// An anti-aliased edge between red and white (pink pixels) given to blue, the
// nearest palette color to them, goes back to red or white.
TEST(TraceQuantizeTest, BlendedEdgesSnapToTheirColors)
{
    auto map = threeColors();
    auto imap = rgbMapQuantize(map, 3);
    unsigned blue = 0;
    for (int i = 0; i < imap.nrColors; i++) {
        if (same(imap.clut[i], BLUE)) {
            blue = i;
        }
    }
    // A one-pixel blend column between the red block and the white around it,
    // quantized to blue.
    for (int y = 5; y < 35; y++) {
        map.setPixel(25, y, {0xd8, 0x80, 0x96});
        imap.setPixel(25, y, blue);
    }
    snapBlendedEdges(map, imap);
    for (int y = 5; y < 35; y++) {
        auto c = imap.getPixelValue(25, y);
        EXPECT_TRUE(same(c, RED) || same(c, WHITE)) << y;
    }
    // Pixels away from the edge are untouched.
    EXPECT_TRUE(same(imap.getPixelValue(10, 20), RED));
    EXPECT_TRUE(same(imap.getPixelValue(45, 20), BLUE));
    EXPECT_TRUE(same(imap.getPixelValue(30, 2), WHITE));
}

// Big noisy images (the fast path with the color cache) give every pixel a valid index.
TEST(TraceQuantizeTest, BigNoisyImage)
{
    std::mt19937 rng(1);
    RgbMap map(1100, 1000);
    for (auto &p : map.pixels) {
        p = {(unsigned char)rng(), (unsigned char)rng(), (unsigned char)rng()};
    }
    auto imap = rgbMapQuantize(map, 8);
    ASSERT_GE(imap.nrColors, 1);
    ASSERT_LE(imap.nrColors, 8);
    for (auto index : imap.pixels) {
        ASSERT_LT(index, (unsigned)imap.nrColors);
    }
}

// Smoothing evens out noise but doesn't mix the colors on two sides of an edge.
TEST(TraceQuantizeTest, SmoothingKeepsEdges)
{
    std::mt19937 rng(2);
    RgbMap map(40, 20);
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 40; x++) {
            int noise = (int)(rng() % 11) - 5; // +-5 levels
            unsigned char v = x < 20 ? 60 + noise : 200 + noise;
            map.setPixel(x, y, {v, v, v});
        }
    }
    auto smooth = rgbMapGaussian(map);
    for (int y = 2; y < 18; y++) {
        // Right next to the edge, each side keeps its own color.
        EXPECT_NEAR(smooth.getPixel(19, y).r, 60, 5) << y;
        EXPECT_NEAR(smooth.getPixel(20, y).r, 200, 5) << y;
        // Noise is reduced.
        EXPECT_NEAR(smooth.getPixel(10, y).r, 60, 3) << y;
    }
}

// A blue logo on white with soft (anti-aliased) edges, quantized to more colors than
// it has: the shades that only exist as rims around the shape are dropped.
TEST(TraceQuantizeTest, RimShadesDropped)
{
    RgbMap map(80, 80);
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 80; x++) {
            // Distance outside a 40x40 square: 0 inside, rising over 3 pixels of soft edge.
            int d = std::max({20 - x, x - 59, 20 - y, y - 59, 0});
            double t = std::min(d, 4) / 4.0; // 0 = blue, 1 = white
            auto mix = [t](int a, int b) { return (unsigned char)(a + (b - a) * t + 0.5); };
            map.setPixel(x, y, {mix(BLUE.r, 255), mix(BLUE.g, 255), mix(BLUE.b, 255)});
        }
    }
    auto imap = rgbMapQuantize(map, 8);
    ASSERT_GT(imap.nrColors, 2);
    dropBlendColors(map, imap);
    ASSERT_EQ(imap.nrColors, 2);
    std::set<unsigned> colors;
    for (int i = 0; i < imap.nrColors; i++) {
        auto c = imap.clut[i];
        colors.insert((c.r << 16) | (c.g << 8) | c.b);
    }
    EXPECT_EQ(colors, (std::set<unsigned>{0x1d3f8f, 0xffffff}));
    for (auto index : imap.pixels) {
        ASSERT_LT(index, 2u);
    }
}

// Colors that fill areas of their own stay, even when they are a mix of two others.
TEST(TraceQuantizeTest, SolidMixKept)
{
    RgbMap map(60, 20);
    RGB const PINK = {0xd8, 0x80, 0x96}; // between red and white
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 60; x++) {
            map.setPixel(x, y, x < 20 ? RED : x < 40 ? PINK : WHITE);
        }
    }
    auto imap = rgbMapQuantize(map, 3);
    dropBlendColors(map, imap);
    EXPECT_EQ(imap.nrColors, 3);
}
