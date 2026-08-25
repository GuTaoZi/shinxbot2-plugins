#include "render.h"

#include <Magick++.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace dateimg_render {
namespace {

std::atomic<uint64_t> g_seq{0}; // uniquifier for temp output files

// Render one text line onto a transparent image sized to the glyphs (+pad).
// NB: font() loads a font FILE by path; fontFamily() would drop CJK glyphs.
Magick::Image make_line(const std::string &text, const std::string &font,
                        double pt, const std::string &color) {
    Magick::Image scratch(Magick::Geometry(8, 8), Magick::Color(0, 0, 0, 0));
    scratch.font(font);
    scratch.fontPointsize(pt);
    Magick::TypeMetric tm;
    scratch.fontTypeMetrics(text, &tm);

    const int pad = static_cast<int>(std::ceil(pt * 0.35));
    const int w =
        std::max(1, static_cast<int>(std::ceil(tm.textWidth())) + pad);
    const int h =
        std::max(1, static_cast<int>(std::ceil(tm.textHeight())) + pad);

    Magick::Image layer(Magick::Geometry(w, h), Magick::Color(0, 0, 0, 0));
    layer.alpha(true);
    layer.font(font);
    layer.fontPointsize(pt);
    layer.fillColor(Magick::Color(color.empty() ? "black" : color));
    layer.textAntiAlias(true);
    layer.annotate(text, Magick::CenterGravity);
    // trim the transparent border so lines stack with a controlled gap (CJK
    // type metrics leave generous leading, which otherwise spreads 2-line dates
    // far apart). repage() clears the virtual-canvas offset trim leaves behind.
    try {
        layer.trim();
        layer.repage();
    } catch (...) {
    }
    return layer;
}

} // namespace

bool render(const RenderSpec &s, std::string &err) {
    try {
        Magick::Image base;
        base.read(s.bg_path);
        base.alpha(true);

        const double pt = s.font_size > 0 ? s.font_size : 48;
        const std::string mfont = s.font_path;
        const std::string lfont =
            s.lunar_font_path.empty() ? s.font_path : s.lunar_font_path;

        // stack: [lunar] then main line(s), all horizontally centered
        std::vector<Magick::Image> lines;
        if (!s.lunar_text.empty()) {
            lines.push_back(make_line(s.lunar_text, lfont, pt * s.lunar_scale,
                                      s.lunar_color));
        }
        lines.push_back(make_line(s.main_line1, mfont, pt, s.color));
        if (s.two_line && !s.main_line2.empty()) {
            lines.push_back(make_line(s.main_line2, mfont, pt, s.color));
        }

        const int gap = static_cast<int>(std::ceil(pt * 0.18));
        size_t cw = 1, ch = 0;
        for (auto &l : lines) {
            cw = std::max(cw, l.columns());
            ch += l.rows();
        }
        if (lines.size() > 1) {
            ch += gap * (lines.size() - 1);
        }

        Magick::Image combined(Magick::Geometry(cw, std::max<size_t>(1, ch)),
                               Magick::Color(0, 0, 0, 0));
        combined.alpha(true);
        ::ssize_t yoff = 0;
        for (auto &l : lines) {
            const ::ssize_t xoff = static_cast<::ssize_t>(
                std::lround((static_cast<double>(cw) - l.columns()) / 2.0));
            combined.composite(l, xoff, yoff, MagickCore::OverCompositeOp);
            yoff += static_cast<::ssize_t>(l.rows()) + gap;
        }

        if (std::fabs(s.rotation) > 0.01) {
            combined.backgroundColor(Magick::Color(0, 0, 0, 0));
            combined.rotate(s.rotation); // degrees clockwise
        }

        const ::ssize_t ox =
            static_cast<::ssize_t>(std::lround(s.x - combined.columns() / 2.0));
        const ::ssize_t oy =
            static_cast<::ssize_t>(std::lround(s.y - combined.rows() / 2.0));
        base.composite(combined, ox, oy, MagickCore::OverCompositeOp);

        if (s.output_max_px > 0) {
            const size_t md = std::max(base.columns(), base.rows());
            if (md > static_cast<size_t>(s.output_max_px)) {
                const double sc = static_cast<double>(s.output_max_px) / md;
                base.resize(
                    Magick::Geometry(static_cast<size_t>(base.columns() * sc),
                                     static_cast<size_t>(base.rows() * sc)));
            }
        }

        base.magick(s.mode == "png" ? "PNG" : "GIF");
        const std::string tmp =
            s.out_path + ".tmp" + std::to_string(g_seq.fetch_add(1));
        base.write(tmp);
        std::error_code ec;
        fs::rename(tmp, s.out_path, ec);
        if (ec) {
            base.write(s.out_path); // fallback (cross-device rename)
            fs::remove(tmp, ec);
        }
        return true;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
}

} // namespace dateimg_render
