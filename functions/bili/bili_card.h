#pragma once

// Push picture for bili updates (new video / dynamic / live on / live off).
//
// History of user requests (2026-11-16, GuTao): the old push (cover image + several text lines in one message)
// "may look separate", so pushes become one drawn picture: the cover with a type tag (新视频/动态/直播中/已下播; user 2026-11-16: "新视频" for pushes, "视频" for decoding) and
// the video length, the title in large type, the UP's avatar, name and time, and a QR code of the link. A QQ card
// was tried first (Bilibili mini-program card via NapCat get_mini_app_ark), but on phones it always opens the
// mini-program, and the user wants the real link; so the push is one message: the picture, then " <link>".
// The picture carries an image `summary` ("你关注的X开播啦：[标题]"), which QQ shows in the chat list instead of
// "[图片]", so the preview reads "你关注的X开播啦：[标题] https://…" (the user preferred one message with the link
// in the preview over two messages without it). Requested sizes: big title (56 px, or 44 px on two
// lines), 36/32 px author row, 44 px tag centred in its pill. Prototype: Python/Pillow, ported here to Magick++.

#include <cstdint>
#include <string>

namespace bili_card {

// video_info (2026-11-16, user: "for video decoding like given a bv code in chat, output the video information,
// I think other plugin has that too"): the reply to a BV/av code, after nonebot-plugin-bilichat's bbot_default
// style: cover with the tag 视频 (not the 分区: user 2026-11-16 "for decoding say 视频") and length, big title + QR, avatar/name/publish date, a row of six stats
// (播放 弹幕 点赞 投币 收藏 分享) and up to 4 lines of 简介. Taller than the 5:4 push picture.
enum class kind_t { video, dynamic, live_on, live_off, video_info };

struct stats_t {
    int64_t view = -1, danmaku = -1, like = -1, coin = -1, favorite = -1, share = -1;
};

struct info_t {
    kind_t kind = kind_t::video;
    std::string up;        // UP name
    std::string title;     // video / live title, or the start of a dynamic's text
    std::string cover_url; // falls back to the avatar when empty (text dynamics)
    std::string face_url;  // avatar
    std::string link;      // what the QR code and the link message point to
    int64_t ts = 0;        // publish time (live: now), shown in Beijing time
    int duration_sec = 0;  // video length, 0 = no badge
    // video_info only:
    std::string category;  // 分区 (tname), shown as the tag
    std::string desc;      // 简介
    stats_t stats;
};

// Draws the picture and writes it under <resource>/bili_cards/. Returns the absolute path, or "" and sets err.
std::string render(const info_t &info, const std::string &resource_dir, std::string &err);

// Chat-list text for the picture, e.g. "你关注的云零子开播啦：[转]".
std::string summary(const info_t &info);

// Deletes pictures in <resource>/bili_cards/ older than max_age_sec. Returns how many were removed.
int prune(const std::string &resource_dir, int64_t max_age_sec = 2 * 24 * 3600);

} // namespace bili_card
