#include "bili_debug.h"

#include "bili_http.h"

#include <sstream>

namespace {

const char *kBiligetDebugBuildTag = "2026-04-17-debug-v9";

std::string fmt_debug_line(const std::string &name, const std::string &host,
                           const std::string &path)
{
    const biliget_http::debug_result_t r =
        biliget_http::debug_endpoint(host, path);

    std::ostringstream oss;
    oss << name << ": ";
    if (!r.has_json) {
        oss << "no_json";
        if (!r.detail.empty()) {
            oss << " detail='" << biliget_http::shorten_text(r.detail, 100)
                << "'";
        }
        return oss.str();
    }

    oss << "code=" << r.code;
    if (!r.message.empty()) {
        oss << " msg='" << biliget_http::shorten_text(r.message, 40) << "'";
    }
    if (r.uid != 0) {
        oss << " uid=" << r.uid;
    }
    if (r.room_id != 0) {
        oss << " room_id=" << r.room_id;
    }
    if (r.live_status >= 0) {
        oss << " live_status=" << r.live_status;
    }
    if (!r.detail.empty()) {
        oss << " detail='" << biliget_http::shorten_text(r.detail, 100) << "'";
    }
    return oss.str();
}

} // namespace

std::string biliget_debug_report(const std::string &id_text)
{
    std::ostringstream oss;
    oss << "[bili.debug] build=" << kBiligetDebugBuildTag
        << " input=" << id_text << "\n"
        << fmt_debug_line("A.getRoomPlayInfo", "https://api.live.bilibili.com",
                          "/xlive/web-room/v1/index/getRoomPlayInfo?room_id=" +
                              id_text)
        << "\n"
        << fmt_debug_line(
               "B.get_status_info_by_uids", "https://api.live.bilibili.com",
               "/room/v1/Room/get_status_info_by_uids?uids%5B%5D=" + id_text)
        << "\n"
        << fmt_debug_line("C.room_init", "https://api.live.bilibili.com",
                          "/room/v1/Room/room_init?id=" + id_text)
        << "\n"
        << fmt_debug_line("D.get_info", "https://api.live.bilibili.com",
                          "/room/v1/Room/get_info?room_id=" + id_text)
        << "\n"
        << fmt_debug_line("E.Master/info", "https://api.live.bilibili.com",
                          "/live_user/v1/Master/info?uid=" + id_text);
    return oss.str();
}
