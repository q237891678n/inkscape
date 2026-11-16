// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Tests for Trace Bitmap's Autotrace engine glue.
 */
#include <algorithm>
#include <cmath>
#include <set>
#include <gdkmm/pixbuf.h>
#include <gdkmm/wrap_init.h>
#include <glibmm/init.h>
#include <gtest/gtest.h>
#include <2geom/pathvector.h>

#include "async/progress.h"
#include "trace/autotrace/inkscape-autotrace.h"

using namespace Inkscape;
using namespace Inkscape::Trace;

namespace {

struct NoProgress final : Async::Progress<double>
{
    bool _keepgoing() const override { return true; }
    bool _report(double const &) override { return true; }
};

// White, a blue ring (an O) with soft edges, and a red dot in the middle of its hole.
Glib::RefPtr<Gdk::Pixbuf> ringImage()
{
    int const size = 120;
    auto pb = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, size, size);
    for (int y = 0; y < size; y++) {
        auto p = pb->get_pixels() + y * pb->get_rowstride();
        for (int x = 0; x < size; x++, p += 3) {
            double d = std::hypot(x + 0.5 - 60, y + 0.5 - 60);
            // coverage of the ring 25..45, with a one-pixel soft edge
            double ring = std::clamp(std::min(d - 24.5, 45.5 - d), 0.0, 1.0);
            double dot = std::clamp(10.5 - d, 0.0, 1.0);
            p[0] = 255 - ring * (255 - 0x1d) - dot * (255 - 0xb2);
            p[1] = 255 - ring * (255 - 0x3f) - dot * 255;
            p[2] = 255 - ring * (255 - 0x8f) - dot * (255 - 0x2d);
        }
    }
    return pb;
}

// Whether a point is inside a fill (nonzero rule) of a path vector.
bool filled(Geom::PathVector const &pv, Geom::Point p)
{
    return pv.winding(p) != 0;
}

} // namespace

TEST(TraceAutotraceTest, HolesAndOrder)
{
    Glib::init();
    Gdk::wrap_init();

    Autotrace::AutotraceTracingEngine engine;
    engine.setColorCount(9); // "Multiple scans" with 8 scans
    engine.setFilterIterations(4);
    engine.setErrorThreshold(2);
    NoProgress progress;
    auto result = engine.trace(ringImage(), progress);

    // Just the ring and the dot: no paths for the soft-edge shades.
    ASSERT_EQ(result.size(), 2u);

    Geom::PathVector const *ring = nullptr, *dot = nullptr;
    for (auto const &r : result) {
        (r.style.find("#1d3f8f") != std::string::npos ? ring : dot) = &r.path; // the dot is the red one
    }
    ASSERT_TRUE(ring && dot);

    // The ring keeps its hole: the inside of the O isn't blue, outside neither.
    EXPECT_TRUE(filled(*ring, {60, 25}));  // on the ring
    EXPECT_FALSE(filled(*ring, {60, 42})); // in the hole, between ring and dot
    EXPECT_FALSE(filled(*ring, {5, 5}));   // outside
    EXPECT_TRUE(filled(*dot, {60, 60}));
}

// The preview shows the image with its colors reduced, as it will be traced (it used
// to be the image unchanged): here white, the ring's blue and the dot's red.
TEST(TraceAutotraceTest, Preview)
{
    Glib::init();
    Gdk::wrap_init();

    Autotrace::AutotraceTracingEngine engine;
    engine.setColorCount(9);
    auto pb = engine.preview(ringImage());
    ASSERT_TRUE(pb);
    ASSERT_EQ(pb->get_n_channels(), 3);
    std::set<unsigned> colors;
    for (int y = 0; y < pb->get_height(); y++) {
        auto p = pb->get_pixels() + y * pb->get_rowstride();
        for (int x = 0; x < pb->get_width(); x++, p += 3) {
            colors.insert((p[0] << 16) | (p[1] << 8) | p[2]);
        }
    }
    EXPECT_EQ(colors.size(), 3u);
    EXPECT_TRUE(colors.count(0xffffff));
    EXPECT_TRUE(colors.count(0x1d3f8f));
}
