// Push picture for bili updates; see bili_card.h for the design and the user requests behind it.

#include "bili_card.h"

#include "bili_qrcodegen.hpp"  // Nayuki QR-Code-generator v1.8.0 (MIT), vendored

#include <Magick++.h>
#include <curl/curl.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <vector>

namespace fs = std::filesystem;

namespace bili_card {
namespace {

constexpr int W = 1000, H = 800, COVER_H = 562, MARGIN = 32, QR = 168;
const char *FONT = "Noto-Sans-CJK-SC";
const char *FONT_B = "Noto-Sans-CJK-SC-Bold";

struct rgb_t {
    int r, g, b;
};
const rgb_t TEXT{24, 25, 28}, MUTED{140, 145, 152}, WHITE{255, 255, 255};

Magick::ColorRGB rgba(rgb_t c, double alpha = 1.0)
{
    Magick::ColorRGB col(c.r / 255.0, c.g / 255.0, c.b / 255.0);
    col.alpha(alpha);
    return col;
}

size_t on_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

bool fetch(const std::string &url, std::string &out)
{
    if (url.empty()) {
        return false;
    }
    std::string u = url.rfind("//", 0) == 0 ? "https:" + url : url;
    CURL *c = curl_easy_init();
    if (!c) {
        return false;
    }
    out.clear();
    curl_easy_setopt(c, CURLOPT_URL, u.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140 Safari/537.36");
    curl_easy_setopt(c, CURLOPT_REFERER, "https://www.bilibili.com/");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    const CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && code == 200 && !out.empty();
}

bool load(const std::string &url, Magick::Image &img)
{
    std::string data;
    if (!fetch(url, data)) {
        return false;
    }
    try {
        img.read(Magick::Blob(data.data(), data.size()));
        img.colorSpace(MagickCore::sRGBColorspace);
        return img.columns() > 0 && img.rows() > 0;
    }
    catch (const Magick::Exception &) {
        return false;
    }
}

// Scale to fill w x h, then crop the middle (like CSS object-fit: cover).
void cover_fit(Magick::Image &img, size_t w, size_t h)
{
    Magick::Geometry g(w, h);
    g.fillArea(true);
    img.resize(g);
    img.crop(Magick::Geometry(w, h, (img.columns() - w) / 2, (img.rows() - h) / 2));
    img.repage();
}

std::vector<std::string> utf8_chars(const std::string &s)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        n = std::min(n, s.size() - i);
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

double text_width(Magick::Image &probe, const char *font, double pt, const std::string &text)
{
    if (text.empty()) {
        return 0;
    }
    probe.font(font);
    probe.fontPointsize(pt);
    probe.textEncoding("UTF-8");
    Magick::TypeMetric m;
    probe.fontTypeMetrics(text, &m);
    return m.textWidth();
}

// Greedy per-character wrap; the last kept line ends in "…" when text is cut.
std::vector<std::string> wrap(Magick::Image &probe, const char *font, double pt, const std::string &text,
                              double width, size_t max_lines)
{
    std::vector<std::string> lines;
    std::string line;
    for (const auto &ch : utf8_chars(text)) {
        if (ch == "\r") {
            continue;
        }
        if (ch == "\n") {
            lines.push_back(line);
            line.clear();
            continue;
        }
        if (!line.empty() && text_width(probe, font, pt, line + ch) > width) {
            lines.push_back(line);
            line = ch;
        }
        else {
            line += ch;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        std::vector<std::string> chars = utf8_chars(lines.back());
        while (!chars.empty()) {
            std::string s;
            for (const auto &c : chars) {
                s += c;
            }
            if (text_width(probe, font, pt, s + "…") <= width) {
                lines.back() = s + "…";
                break;
            }
            chars.pop_back();
        }
    }
    return lines;
}

// Noto Sans CJK ink sits about 0.37 em above the baseline at its middle, for CJK and for digits alike.
double baseline_for_middle(double mid, double pt) { return mid + 0.37 * pt; }

void text(Magick::Image &img, const char *font, double pt, rgb_t color, double x, double baseline,
          const std::string &s)
{
    std::vector<Magick::Drawable> d{Magick::DrawableFont(font), Magick::DrawablePointSize(pt),
                                    Magick::DrawableFillColor(rgba(color)),
                                    Magick::DrawableStrokeColor(Magick::Color("none")),
                                    Magick::DrawableText(x, baseline, s, "UTF-8")};
    img.draw(d);
}

// Rounded pill with its text centred both ways. Returns the pill's width.
double pill(Magick::Image &img, double x, double y, const std::string &s, double pt, rgb_t bg, double bg_alpha,
            double pad_x, double pad_y)
{
    const double tw = text_width(img, FONT_B, pt, s);
    const double h = pt + 2 * pad_y + 6, w = tw + 2 * pad_x;
    std::vector<Magick::Drawable> d{Magick::DrawableFillColor(rgba(bg, bg_alpha)),
                                    Magick::DrawableStrokeColor(Magick::Color("none")),
                                    Magick::DrawableRoundRectangle(x, y, x + w, y + h, h / 2, h / 2)};
    img.draw(d);
    text(img, FONT_B, pt, WHITE, x + pad_x, baseline_for_middle(y + h / 2, pt), s);
    return w;
}

Magick::Image qr_image(const std::string &link)
{
    const auto q = qrcodegen::QrCode::encodeText(link.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int border = 1, n = q.getSize(), s = n + 2 * border;
    Magick::Image img(Magick::Geometry(s, s), Magick::Color("white"));
    img.modifyImage();
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (q.getModule(x, y)) {
                img.pixelColor(x + border, y + border, Magick::Color("black"));
            }
        }
    }
    Magick::Geometry g(QR, QR);
    g.aspect(true);
    img.sample(g); // nearest neighbour: crisp modules
    return img;
}

std::string beijing_time(int64_t ts)
{
    std::time_t t = static_cast<std::time_t>((ts > 0 ? ts : std::time(nullptr)) + 8 * 3600);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm);
    return buf;
}

std::string clip(const std::string &s, size_t max_chars)
{
    auto chars = utf8_chars(s);
    if (chars.size() <= max_chars) {
        return s;
    }
    std::string out;
    for (size_t i = 0; i < max_chars; ++i) {
        out += chars[i];
    }
    return out + "…";
}

} // namespace

std::string summary(const info_t &info)
{
    std::string flat;
    for (const auto &ch : utf8_chars(info.title)) {
        flat += (ch == "\n" || ch == "\r" || ch == "\t") ? " " : ch;
    }
    const std::string title = clip(flat, 40);
    // User, 2026-11-16: one message (picture, then " link"); the preview reads "你关注的X开播啦：[标题] https://…",
    // so the title is bracketed to keep it apart from the link.
    const std::string tail = title.empty() ? "" : "：[" + title + "]";
    switch (info.kind) {
    case kind_t::video:
        return "你关注的" + info.up + "发布新视频啦" + tail;
    case kind_t::dynamic:
        return "你关注的" + info.up + "发新动态啦" + tail;
    case kind_t::live_on:
        return "你关注的" + info.up + "开播啦" + tail;
    case kind_t::live_off:
        return "你关注的" + info.up + "刚刚下播啦" + tail;
    case kind_t::video_info:
        return info.up + "的视频" + tail;
    }
    return info.up;
}

namespace {

std::string duration_text(int sec)
{
    const int m = sec / 60, s = sec % 60;
    return m >= 60 ? fmt::format("{}:{:02}:{:02}", m / 60, m % 60, s) : fmt::format("{}:{:02}", m, s);
}

// 12345 -> "1.2万", 123456789 -> "1.2亿"; -1 (unknown) -> "-".
std::string human(int64_t n)
{
    if (n < 0) {
        return "-";
    }
    if (n >= 100000000) {
        return fmt::format("{:.1f}亿", n / 1e8);
    }
    if (n >= 10000) {
        return fmt::format("{:.1f}万", n / 1e4);
    }
    return std::to_string(n);
}

void paste_avatar(Magick::Image &canvas, Magick::Image face, double size, double x, double y)
{
    Magick::Geometry g(size, size);
    g.aspect(true);
    face.resize(g);
    face.alpha(true);
    Magick::Image mask(Magick::Geometry(size, size), Magick::Color("black"));
    mask.alpha(false);
    mask.draw(std::vector<Magick::Drawable>{
        Magick::DrawableFillColor(Magick::Color("white")),
        Magick::DrawableEllipse((size - 1) / 2.0, (size - 1) / 2.0, (size - 1) / 2.0, (size - 1) / 2.0, 0, 360)});
    face.composite(mask, 0, 0, MagickCore::CopyAlphaCompositeOp);
    canvas.composite(face, static_cast<ssize_t>(x), static_cast<ssize_t>(y), MagickCore::OverCompositeOp);
}

std::string save(Magick::Image &canvas, const info_t &info, const std::string &resource_dir)
{
    const fs::path dir = fs::absolute(fs::path(resource_dir) / "bili_cards");
    fs::create_directories(dir);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path out =
        dir / fmt::format("{}_{}_{:x}.jpg", static_cast<int>(info.kind), std::time(nullptr),
                          std::hash<std::string>{}(info.up + info.title + std::to_string(stamp)));
    canvas.magick("JPEG");
    canvas.quality(92);
    canvas.write(out.string());
    return out.string();
}

// The reply to a BV/av code (see bili_card.h). Height grows with the title and 简介.
std::string render_info(const info_t &info, const std::string &resource_dir, std::string &err)
{
    Magick::Image cover, face;
    const bool have_face = load(info.face_url, face);
    if (!load(info.cover_url, cover)) {
        err = "cover download failed";
        return "";
    }
    cover_fit(cover, W, COVER_H);

    Magick::Image probe(Magick::Geometry(8, 8), Magick::Color("white"));
    const double text_w = W - 2 * MARGIN - QR - 28;
    double pt = 52;
    std::vector<std::string> title = wrap(probe, FONT_B, pt, info.title, text_w, 1);
    if (text_width(probe, FONT_B, pt, info.title) > text_w) {
        pt = 42;
        title = wrap(probe, FONT_B, pt, info.title, text_w, 2);
    }
    const double lh = std::round(pt * 1.3), row_h = 60;
    const double head_h = std::max<double>(QR, title.size() * lh + 14 + row_h);
    std::string desc;
    for (const auto &ch : utf8_chars(info.desc)) {
        desc += ch == "\r" ? "" : ch;
    }
    while (!desc.empty() && (desc.back() == '\n' || desc.back() == ' ')) {
        desc.pop_back();
    }
    const double dpt = 28, dlh = std::round(dpt * 1.45);
    const std::vector<std::string> dlines =
        desc.empty() ? std::vector<std::string>{} : wrap(probe, FONT, dpt, desc, W - 2 * MARGIN, 4);
    const double stats_h = 104;
    const double top = COVER_H + 28;
    const double height = top + head_h + 24 + stats_h + (dlines.empty() ? 0 : 20 + dlines.size() * dlh) + 32;

    Magick::Image canvas(Magick::Geometry(W, static_cast<size_t>(height)), Magick::Color("white"));
    canvas.composite(cover, 0, 0, MagickCore::OverCompositeOp);
    pill(canvas, 30, 28, std::string("视频"), 44,
         {251, 114, 153}, 235 / 255.0, 24, 8);
    if (info.duration_sec > 0) {
        const std::string dur = duration_text(info.duration_sec);
        pill(canvas, W - 30 - text_width(canvas, FONT_B, 34, dur) - 40, COVER_H - 80, dur, 34, {0, 0, 0},
             170 / 255.0, 20, 6);
    }

    if (!info.link.empty()) {
        canvas.composite(qr_image(info.link), W - MARGIN - QR, static_cast<ssize_t>(top), MagickCore::OverCompositeOp);
    }
    double y = top + (head_h - (title.size() * lh + 14 + row_h)) / 2;
    for (const auto &line : title) {
        text(canvas, FONT_B, pt, TEXT, MARGIN, baseline_for_middle(y + lh / 2, pt), line);
        y += lh;
    }
    y += 14;
    double nx = MARGIN;
    if (have_face) {
        paste_avatar(canvas, face, row_h, MARGIN, y);
        nx += row_h + 14;
    }
    std::string when;
    {
        std::time_t t = static_cast<std::time_t>((info.ts > 0 ? info.ts : std::time(nullptr)) + 8 * 3600);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        when = buf;
    }
    const double meta_w = text_width(canvas, FONT, 30, "  " + when);
    const auto name_l = wrap(canvas, FONT_B, 34, info.up, std::max(120.0, MARGIN + text_w - nx - meta_w), 1);
    const std::string name = name_l.empty() ? "" : name_l[0];
    const double mid = y + row_h / 2;
    text(canvas, FONT_B, 34, TEXT, nx, baseline_for_middle(mid, 34), name);
    text(canvas, FONT, 30, MUTED, nx + text_width(canvas, FONT_B, 34, name) + 16, baseline_for_middle(mid + 2, 30),
         when);

    y = top + head_h + 24;
    canvas.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor(rgba({241, 242, 244})),
                                              Magick::DrawableStrokeColor(Magick::Color("none")),
                                              Magick::DrawableRoundRectangle(MARGIN, y, W - MARGIN, y + stats_h, 20, 20)});
    const std::pair<const char *, int64_t> cells[] = {
        {"播放", info.stats.view},     {"弹幕", info.stats.danmaku},  {"点赞", info.stats.like},
        {"投币", info.stats.coin},     {"收藏", info.stats.favorite}, {"分享", info.stats.share}};
    const double cw = (W - 2.0 * MARGIN) / 6;
    for (int i = 0; i < 6; ++i) {
        const double cx = MARGIN + cw * i + cw / 2;
        const std::string v = human(cells[i].second);
        text(canvas, FONT_B, 34, TEXT, cx - text_width(canvas, FONT_B, 34, v) / 2,
             baseline_for_middle(y + 36, 34), v);
        text(canvas, FONT, 24, MUTED, cx - text_width(canvas, FONT, 24, cells[i].first) / 2,
             baseline_for_middle(y + 76, 24), cells[i].first);
    }
    y += stats_h + 20;
    for (const auto &line : dlines) {
        text(canvas, FONT, dpt, {90, 94, 102}, MARGIN, baseline_for_middle(y + dlh / 2, dpt), line);
        y += dlh;
    }
    return save(canvas, info, resource_dir);
}

} // namespace

std::string render(const info_t &info, const std::string &resource_dir, std::string &err)
{
    try {
        if (info.kind == kind_t::video_info) {
            return render_info(info, resource_dir, err);
        }
        Magick::Image canvas(Magick::Geometry(W, H), Magick::Color("white"));

        Magick::Image cover, face;
        const bool have_face = load(info.face_url, face);
        if (!load(info.cover_url, cover)) {
            if (!have_face) {
                err = "cover and avatar download failed";
                return "";
            }
            cover = face; // text dynamics have no cover: use the avatar
        }
        cover_fit(cover, W, COVER_H);
        if (info.kind == kind_t::live_off) {
            cover.colorize(45, rgba({60, 60, 60}));
        }
        canvas.composite(cover, 0, 0, MagickCore::OverCompositeOp);

        struct tag_t {
            const char *text;
            rgb_t color;
        };
        const tag_t tag = info.kind == kind_t::video     ? tag_t{"新视频", {251, 114, 153}}
                          : info.kind == kind_t::dynamic ? tag_t{"动态", {0, 174, 236}}
                          : info.kind == kind_t::live_on ? tag_t{"直播中", {255, 69, 58}}
                                                         : tag_t{"已下播", {120, 124, 132}};
        pill(canvas, 30, 28, tag.text, 44, tag.color, 235 / 255.0, 24, 8);
        if (info.kind == kind_t::video && info.duration_sec > 0) {
            const int m = info.duration_sec / 60, s = info.duration_sec % 60;
            const std::string dur = m >= 60 ? fmt::format("{}:{:02}:{:02}", m / 60, m % 60, s)
                                            : fmt::format("{}:{:02}", m, s);
            const double tw = text_width(canvas, FONT_B, 34, dur);
            pill(canvas, W - 30 - tw - 40, COVER_H - 80, dur, 34, {0, 0, 0}, 170 / 255.0, 20, 6);
        }

        const bool with_qr = !info.link.empty();
        if (with_qr) {
            canvas.composite(qr_image(info.link), W - MARGIN - QR, COVER_H + (H - COVER_H - QR) / 2,
                             MagickCore::OverCompositeOp);
        }
        const double text_w = W - 2 * MARGIN - (with_qr ? QR + 28 : 0);

        double pt = 56;
        std::vector<std::string> lines = wrap(canvas, FONT_B, pt, info.title, text_w, 1);
        if (text_width(canvas, FONT_B, pt, info.title) > text_w) {
            pt = 44;
            lines = wrap(canvas, FONT_B, pt, info.title, text_w, 2);
        }
        const double lh = std::round(pt * 1.3), row_h = 60;
        const double block = lines.size() * lh + 16 + row_h;
        double y = COVER_H + (H - COVER_H - block) / 2 - 6;
        for (const auto &line : lines) {
            text(canvas, FONT_B, pt, TEXT, MARGIN, baseline_for_middle(y + lh / 2, pt), line);
            y += lh;
        }
        y += 16;

        const double mid = y + row_h / 2;
        double nx = MARGIN;
        if (have_face) {
            Magick::Geometry g(row_h, row_h);
            g.aspect(true);
            face.resize(g);
            face.alpha(true);
            Magick::Image mask(Magick::Geometry(row_h, row_h), Magick::Color("black"));
            mask.alpha(false);
            mask.draw(std::vector<Magick::Drawable>{
                Magick::DrawableFillColor(Magick::Color("white")),
                Magick::DrawableEllipse((row_h - 1) / 2.0, (row_h - 1) / 2.0, (row_h - 1) / 2.0,
                                        (row_h - 1) / 2.0, 0, 360)});
            face.composite(mask, 0, 0, MagickCore::CopyAlphaCompositeOp);
            canvas.composite(face, MARGIN, static_cast<ssize_t>(y), MagickCore::OverCompositeOp);
            nx += row_h + 14;
        }
        const std::string when = beijing_time(info.ts);
        const double meta_w = text_width(canvas, FONT, 32, "  " + when);
        const double room = std::max(120.0, MARGIN + text_w - nx - meta_w);
        const auto name_lines = wrap(canvas, FONT_B, 36, info.up, room, 1);
        const std::string name = name_lines.empty() ? "" : name_lines[0];
        text(canvas, FONT_B, 36, TEXT, nx, baseline_for_middle(mid, 36), name);
        text(canvas, FONT, 32, MUTED, nx + text_width(canvas, FONT_B, 36, name) + 16,
             baseline_for_middle(mid + 2, 32), when);

        const fs::path dir = fs::absolute(fs::path(resource_dir) / "bili_cards");
        fs::create_directories(dir);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path out =
            dir / fmt::format("{}_{}_{:x}.jpg", static_cast<int>(info.kind), std::time(nullptr),
                              std::hash<std::string>{}(info.up + info.title + std::to_string(stamp)));
        canvas.magick("JPEG");
        canvas.quality(92);
        canvas.write(out.string());
        return out.string();
    }
    catch (const std::exception &e) {
        err = e.what();
        return "";
    }
}

int prune(const std::string &resource_dir, int64_t max_age_sec)
{
    int removed = 0;
    std::error_code ec;
    const fs::path dir = fs::path(resource_dir) / "bili_cards";
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }
    const auto now = fs::file_time_type::clock::now();
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".jpg") {
            continue;
        }
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - e.last_write_time(ec)).count();
        if (age > max_age_sec && fs::remove(e.path(), ec)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace bili_card
