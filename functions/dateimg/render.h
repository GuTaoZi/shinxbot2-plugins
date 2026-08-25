// render.h — standalone date-sign render engine for the dateimg plugin.
//
// Pure image work (no plugin/bot state) so it can be unit-tested from a CLI
// harness. Composites a (optional) lunar line + the Gregorian date (one or two
// lines) as one centered, optionally-rotated block onto a background, then
// writes a single-frame GIF or a PNG. The anchor (x,y) is the CENTER of the
// whole rendered text block, so it stays put for any date length.
#pragma once

#include <string>

namespace dateimg_render {

struct RenderSpec {
    std::string bg_path;   // background image (png/jpg/…), read by Magick
    std::string font_path; // ABS path to the main date font file
    std::string lunar_font_path; // ABS path for the lunar line; "" => font_path

    double x = 0;                // block center X on the background (px)
    double y = 0;                // block center Y (px)
    double font_size = 48;       // main date point size
    double rotation = 0;         // degrees clockwise (matches the sign tilt)
    std::string color = "black"; // main date color

    bool two_line = false;  // square/circle signs: stack the date on 2 lines
    std::string main_line1; // two_line: "8月"; single: the whole "8月8日"
    std::string main_line2; // two_line: "8日"; single: unused

    std::string lunar_text; // e.g. "拾"; "" => no lunar line
    std::string lunar_color = "red";
    double lunar_scale = 0.5; // lunar point size = font_size * this

    int output_max_px = 1280; // downscale so max(w,h) <= this; <=0 disables
    std::string mode = "gif"; // "gif" (sticker) | "png" (image)
    std::string out_path;     // absolute output path (extension matches mode)
};

// Render per spec, writing spec.out_path atomically. Returns true on success;
// on failure returns false and sets err.
bool render(const RenderSpec &spec, std::string &err);

} // namespace dateimg_render
