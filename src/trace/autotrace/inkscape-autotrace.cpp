// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * This is the C++ glue between Inkscape and Autotrace
 *//*
 *
 * Authors:
 *   Marc Jeanmougin
 *
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */
#include "inkscape-autotrace.h"

#include <cstdlib>
#include <deque>
#include <iomanip>
#include <glibmm/i18n.h>
#include <2geom/path-sink.h>

#include "async/progress.h"
#include "trace/filterset.h"
#include "trace/imagemap.h"
#include "trace/quantize.h"
#include "util/safe-printf.h"

extern "C" {
#include <autotrace/autotrace.h>
#include <autotrace/output.h>
#include <autotrace/spline.h>
}

namespace Inkscape::Trace::Autotrace {
namespace {

struct at_splines_deleter { void operator()(at_splines_type *p) { at_splines_free(p); }; };
using at_splines_uniqptr = std::unique_ptr<at_splines_type, at_splines_deleter>;

struct at_fitting_opts_deleter
{
    void operator()(at_fitting_opts_type *p) { at_fitting_opts_free(p); };
};
using at_fitting_opts_uniqptr = std::unique_ptr<at_fitting_opts_type, at_fitting_opts_deleter>;

/**
 * Eliminate the alpha channel by overlaying on top of white, and ensure the result is in packed RGB8 format.
 * If nothing needs to be done, the original pixbuf is returned, otherwise a new pixbuf is returned.
 */
Glib::RefPtr<Gdk::Pixbuf> to_rgb8_packed(Glib::RefPtr<Gdk::Pixbuf> const &pixbuf)
{
    int width     = pixbuf->get_width();
    int height    = pixbuf->get_height();
    int rowstride = pixbuf->get_rowstride();
    int nchannels = pixbuf->get_n_channels();
    auto data     = pixbuf->get_pixels();

    if (nchannels == 3 && rowstride == width * 3) {
        return pixbuf;
    }

    int imgsize = width * height;
    auto out = new unsigned char[3 * imgsize];
    auto q = out;

    for (int y = 0; y < height; y++) {
        auto p = data + rowstride * y;
        for (int x = 0; x < width; x++) {
            unsigned char alpha = nchannels == 3 ? 255 : p[3];
            unsigned char white = 255 - alpha;
            for (int c = 0; c < 3; c++) {
                // Divide by 255, not 256: that darkened every pixel, so white came out as 254.
                *(q++) = ((int)p[c] * alpha + 127) / 255 + white;
            }
            p += nchannels;
        }
    }

    return Gdk::Pixbuf::create_from_data(out, Gdk::Colorspace::RGB, false, 8, width, height, width * 3, [out] (auto) { delete [] out; });
}

/**
 * Reduce a packed RGB8 pixbuf to at most <ncolors> colors with the quantizer that
 * Potrace's multiple scans use, instead of Autotrace's own: edge-preserving smoothing,
 * exact colors, and none of the in-between shades of anti-aliased edges, which
 * Autotrace traced as thousands of thin fragments around every shape. A palette color
 * within a few levels of <background> becomes exactly <background>, so that Autotrace
 * leaves it out (white with JPEG noise, for example). Only that close: Autotrace cuts
 * background areas out of the regions around them, and the regions under those show
 * through, so a light color made background turned white areas into holes.
 */
Glib::RefPtr<Gdk::Pixbuf> prequantize(Glib::RefPtr<Gdk::Pixbuf> const &pb, int ncolors, at_color background)
{
    int width = pb->get_width();
    int height = pb->get_height();
    auto data = pb->get_pixels(); // packed RGB8, see to_rgb8_packed

    auto map = RgbMap(width, height);
    for (size_t i = 0; i < map.pixels.size(); i++) {
        map.pixels[i] = {data[3 * i], data[3 * i + 1], data[3 * i + 2]};
    }

    auto smoothed = rgbMapGaussian(map);
    auto imap = rgbMapQuantize(smoothed, ncolors);
    dropBlendColors(smoothed, imap);
    snapBlendedEdges(smoothed, imap);
    refinePalette(map, imap);

    constexpr int NEAR_BACKGROUND = 10; // levels per channel
    for (int k = 0; k < imap.nrColors; k++) {
        auto &c = imap.clut[k];
        if (std::abs(c.r - background.r) <= NEAR_BACKGROUND && std::abs(c.g - background.g) <= NEAR_BACKGROUND &&
            std::abs(c.b - background.b) <= NEAR_BACKGROUND) {
            c = {background.r, background.g, background.b};
        }
    }

    auto out = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, width, height);
    for (int y = 0; y < height; y++) {
        auto q = out->get_pixels() + (size_t)y * out->get_rowstride();
        for (int x = 0; x < width; x++) {
            auto c = imap.getPixelValue(x, y);
            *q++ = c.r;
            *q++ = c.g;
            *q++ = c.b;
        }
    }
    return out;
}

} // namespace

AutotraceTracingEngine::AutotraceTracingEngine()
{
    // Create options struct, automatically filled with defaults.
    opts = at_fitting_opts_new();
    opts->background_color = at_color_new(255, 255, 255);
    autotrace_init();
}

AutotraceTracingEngine::~AutotraceTracingEngine()
{
    at_fitting_opts_free(opts);
}

Glib::RefPtr<Gdk::Pixbuf> AutotraceTracingEngine::preview(Glib::RefPtr<Gdk::Pixbuf> const &pixbuf)
{
    // The image with its colors reduced, exactly as Autotrace will trace it (see trace()).
    auto pb = to_rgb8_packed(pixbuf);
    if (opts->color_count > 0) {
        pb = prequantize(pb, opts->color_count,
                         opts->background_color ? *opts->background_color : at_color{255, 255, 255});
    }
    return pb;
}

TraceResult AutotraceTracingEngine::trace(Glib::RefPtr<Gdk::Pixbuf> const &pixbuf, Async::Progress<double> &progress)
{
    auto pb = to_rgb8_packed(pixbuf);

    // Reduce the colors here rather than in Autotrace (see prequantize), and tell it
    // not to reduce them again.
    auto runopts = at_fitting_opts_uniqptr(at_fitting_opts_copy(opts));
    if (opts->color_count > 0) {
        pb = prequantize(pb, opts->color_count,
                         opts->background_color ? *opts->background_color : at_color{255, 255, 255});
        runopts->color_count = 0;
    }
    if (pb->get_rowstride() != pb->get_width() * 3) {
        pb = to_rgb8_packed(pb);
    }

    at_bitmap bitmap;
    bitmap.height = pb->get_height();
    bitmap.width  = pb->get_width();
    bitmap.bitmap = pb->get_pixels();
    bitmap.np     = 3;

    auto throttled = Async::ProgressStepThrottler(progress, 0.02);
    auto sub_trace = Async::SubProgress(throttled, 0.0, 0.8);

    auto splines = at_splines_uniqptr(at_splines_new_full(
        &bitmap, runopts.get(), nullptr, nullptr,
        [](gfloat frac, gpointer data) { reinterpret_cast<decltype(sub_trace) *>(data)->report(frac); }, &sub_trace,
        [](gpointer data) -> gboolean { return !reinterpret_cast<decltype(sub_trace) *>(data)->keepgoing(); },
        &sub_trace));
    // at_output_write_func wfunc = at_output_get_handler_by_suffix("svg");
    // at_spline_writer *wfunc = at_output_get_handler_by_suffix("svg");
    // at_splines_write(wfunc, stdout, "", NULL, splines, NULL, NULL);

    sub_trace.report_or_throw(1.0);
    auto sub_convert = Async::SubProgress(throttled, 0.8, 0.2);

    if (!splines) {
        return {};
    }

    int height = splines->height;

    auto get_style = [&](at_spline_list_type const &list) {
        char color[10];
        safeprintf(color, "#%02x%02x%02x;", list.color.r, list.color.g, list.color.b);

        std::stringstream ss;
        ss << (splines->centerline || list.open ? "stroke:" : "fill:") << color
           << (splines->centerline || list.open ? "fill:" : "stroke:") << "none";

        return ss.str();
    };

    auto to_geom = [=] (at_real_coord const &c) {
        return Geom::Point(c.x, height - c.y);
    };

    // Autotrace's regions are drawn in order, each over the ones before it, and only
    // background areas are cut out of them: the outline of such a hole (the inside of
    // an O) comes as an outline of its own, of the region's color, running the other
    // way. It only makes a hole when it is in the same path as the outline around it,
    // but only outlines that came one after another used to be joined, so any
    // outline in between (often a sliver of an edge shade) made the hole a filled
    // shape of its own: holes filled in, and a big region's holes could cover the
    // whole picture. Each hole now joins the path of the latest region of its color
    // around it.
    struct Item
    {
        explicit Item(std::string style_)
            : style(std::move(style_))
        {}
        Item(Item const &) = delete; // a PathBuilder points into itself: never copy or move one
        std::string style;
        Geom::PathBuilder builder;
        Geom::OptRect box; // of everything in it
    };
    struct Outer
    {
        std::string style;
        Geom::Rect box;
        size_t item;
    };
    std::deque<Item> items; // stays in place as it grows
    std::vector<Outer> outers;

    int const num_splines = SPLINE_LIST_ARRAY_LENGTH(*splines);
    for (int list_i = 0; list_i < num_splines; list_i++) {
        sub_convert.report_or_throw((double)list_i / num_splines);

        auto const &list = SPLINE_LIST_ARRAY_ELT(*splines, list_i);
        if (SPLINE_LIST_LENGTH(list) == 0) {
            continue;
        }

        auto style = get_style(list);
        bool const closed = !(splines->centerline || list.open);

        // The outline's box, from its points (enough to tell which region holds a hole).
        Geom::Rect box(to_geom(START_POINT(SPLINE_LIST_ELT(list, 0))), to_geom(START_POINT(SPLINE_LIST_ELT(list, 0))));
        for (int spline_i = 0; spline_i < SPLINE_LIST_LENGTH(list); spline_i++) {
            auto const spline = SPLINE_LIST_ELT(list, spline_i);
            box.expandTo(to_geom(END_POINT(spline)));
            if (SPLINE_DEGREE(spline) != AT_LINEARTYPE) {
                box.expandTo(to_geom(CONTROL1(spline)));
                box.expandTo(to_geom(CONTROL2(spline)));
            }
        }

        Item *item = nullptr;
        if (closed && list.clockwise) {
            // A hole: the latest region of the same style whose box holds it.
            for (auto it = outers.rbegin(); it != outers.rend(); ++it) {
                if (it->style == style && it->box.contains(box)) {
                    item = &items[it->item];
                    break;
                }
            }
        }
        if (!item) {
            // Join the latest path of the same style if nothing drawn after it overlaps
            // this outline: drawn lower down, it looks the same. Without this, a
            // picture with many colors made a path for nearly every region, ten
            // thousand and more objects for Inkscape to handle.
            size_t index = items.size();
            for (size_t k = items.size(); k-- > 0;) {
                if (items[k].style == style) {
                    index = k;
                    break;
                }
                if (items[k].box && items[k].box->intersects(box)) {
                    break;
                }
            }
            if (index == items.size()) {
                items.emplace_back(style);
            }
            item = &items[index];
            if (closed && !list.clockwise) {
                outers.push_back({style, box, index});
            }
        }
        item->box.unionWith(box);
        auto &pathbuilder = item->builder;

        auto const first = SPLINE_LIST_ELT(list, 0);
        pathbuilder.moveTo(to_geom(START_POINT(first)));

        for (int spline_i = 0; spline_i < SPLINE_LIST_LENGTH(list); spline_i++) {
            auto const spline = SPLINE_LIST_ELT(list, spline_i);

            if (SPLINE_DEGREE(spline) == AT_LINEARTYPE) {
                pathbuilder.lineTo(to_geom(END_POINT(spline)));
            } else {
                pathbuilder.curveTo(to_geom(CONTROL1(spline)), to_geom(CONTROL2(spline)), to_geom(END_POINT(spline)));
            }
        }

        if (closed) {
            pathbuilder.closePath();
        } else {
            pathbuilder.flush();
        }
    }

    // The caller puts each result below the ones before it, so the first one ends up
    // on top: give them in the reverse of Autotrace's drawing order.
    TraceResult res;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        it->builder.flush();
        res.emplace_back(it->style, it->builder.peek());
    }

    return res;
}

void AutotraceTracingEngine::setColorCount(unsigned color_count)
{
    opts->color_count = color_count;
}

void AutotraceTracingEngine::setCenterLine(bool centerline)
{
    opts->centerline = centerline;
}

void AutotraceTracingEngine::setPreserveWidth(bool preserve_width)
{
    opts->preserve_width = preserve_width;
}

void AutotraceTracingEngine::setFilterIterations(unsigned filter_iterations)
{
    opts->filter_iterations = filter_iterations;
}

void AutotraceTracingEngine::setErrorThreshold(float error_threshold)
{
    opts->error_threshold = error_threshold;
}

} // namespace Inkscape::Trace::Autotrace

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
