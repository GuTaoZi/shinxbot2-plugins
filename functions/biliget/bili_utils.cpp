#include "bili_utils.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <limits>
#include <sstream>

namespace biliget_utils {

bool str_all_digits(const std::string &s)
{
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

int64_t json_to_i64(const Json::Value &v, int64_t def)
{
    if (v.isInt64()) {
        return v.asInt64();
    }
    if (v.isUInt64()) {
        const uint64_t u = v.asUInt64();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(u);
    }
    if (v.isInt()) {
        return static_cast<int64_t>(v.asInt());
    }
    if (v.isUInt()) {
        return static_cast<int64_t>(v.asUInt());
    }
    if (v.isDouble()) {
        return static_cast<int64_t>(v.asDouble());
    }
    if (v.isString()) {
        const std::string s = v.asString();
        if (s.empty()) {
            return def;
        }
        try {
            size_t pos = 0;
            const long long val = std::stoll(s, &pos, 10);
            if (pos == s.size()) {
                return static_cast<int64_t>(val);
            }
        }
        catch (...) {
        }
    }
    return def;
}

std::string first_non_empty(const std::initializer_list<std::string> &cands)
{
    auto it = std::find_if(cands.begin(), cands.end(),
                           [](const std::string &s) { return !s.empty(); });
    if (it != cands.end()) {
        return *it;
    }
    return "";
}

std::string url_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out.push_back(static_cast<char>(ch));
        }
        else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0xF]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

std::string compact_cover_url_169(const std::string &url)
{
    if (url.empty()) {
        return "";
    }
    if (url.find("hdslb.com") == std::string::npos) {
        return url;
    }
    if (url.find('@') != std::string::npos) {
        return url;
    }
    return url + "@480w_270h_1c.jpg";
}

std::string compact_cover_url_keep_ratio(const std::string &url)
{
    if (url.empty()) {
        return "";
    }
    // Keep original URL to avoid distorted aspect ratios for dynamic images.
    return url;
}

std::string compact_cover_url_square(const std::string &url)
{
    if (url.empty()) {
        return "";
    }
    if (url.find("hdslb.com") == std::string::npos) {
        return url;
    }
    if (url.find('@') != std::string::npos) {
        return url;
    }
    return url + "@480w_480h_1c.jpg";
}

std::string compact_cover_url_avatar(const std::string &url)
{
    if (url.empty()) {
        return "";
    }
    if (url.find("hdslb.com") == std::string::npos) {
        return url;
    }
    if (url.find('@') != std::string::npos) {
        return url;
    }
    return url + "@270w_270h_1c.jpg";
}

std::string up_name_with_uid(userid_t uid, const std::string &name)
{
    if (name.empty()) {
        return std::to_string(uid);
    }
    return name + " (" + std::to_string(uid) + ")";
}

std::string live_jump_link(userid_t uid, uint64_t room_id)
{
    if (room_id != 0) {
        return "https://live.bilibili.com/" + std::to_string(room_id);
    }
    return "https://space.bilibili.com/" + std::to_string(uid);
}

std::string join_tokens(const std::vector<std::string> &tokens)
{
    std::ostringstream oss;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << tokens[i];
    }
    return oss.str();
}

bool str_to_u64_if_digits(const std::string &s, uint64_t &out)
{
    if (!str_all_digits(s)) {
        return false;
    }
    out = static_cast<uint64_t>(std::stoull(s));
    return true;
}

bool is_dynamic_id_newer_than(const std::string &curr, const std::string &prev)
{
    if (curr.empty()) {
        return false;
    }
    if (prev.empty()) {
        return true;
    }

    uint64_t curr_num = 0;
    uint64_t prev_num = 0;
    const bool curr_is_num = str_to_u64_if_digits(curr, curr_num);
    const bool prev_is_num = str_to_u64_if_digits(prev, prev_num);
    if (curr_is_num && prev_is_num) {
        return curr_num > prev_num;
    }

    if (curr.size() != prev.size()) {
        return curr.size() > prev.size();
    }
    return curr > prev;
}

bool is_fresh_publish_ts(int64_t ts, std::time_t now, int64_t fresh_window_sec)
{
    if (ts <= 0) {
        return false;
    }
    const int64_t now64 = static_cast<int64_t>(now);
    if (ts > now64 + 300) {
        return false;
    }
    return (now64 - ts) <= fresh_window_sec;
}

std::string api_fail_reason(const Json::Value &root,
                            const std::string &api_name)
{
    if (!root.isObject()) {
        return api_name + " 无响应";
    }
    const int code = root.get("code", -9999).asInt();
    if (code == 0) {
        return "";
    }
    const std::string msg = root.get("message", root.get("msg", "")).asString();
    if (code == -412) {
        return api_name + " 被风控拦截(-412)";
    }
    if (code == -799) {
        return api_name + " 触发频控(-799)";
    }
    if (code == -101) {
        return api_name + " 需登录凭据(-101)";
    }
    if (msg.empty()) {
        return api_name + " code=" + std::to_string(code);
    }
    return api_name + " code=" + std::to_string(code) + "(" + msg + ")";
}

std::string mask_cookie_text(const std::string &cookie)
{
    if (cookie.empty()) {
        return "(empty)";
    }
    if (cookie.size() <= 12) {
        return "***";
    }
    return cookie.substr(0, 6) + "..." + cookie.substr(cookie.size() - 6);
}

bool send_group_msg_checked(bot *p, groupid_t gid, const std::string &message,
                            std::string &err)
{
    if (p == nullptr) {
        err = "bot=null";
        return false;
    }

    Json::Value req(Json::objectValue);
    req["group_id"] = Json::UInt64(gid);
    req["message"] = trim(message);

    const std::string resp = p->cq_send("send_group_msg", req);
    const Json::Value root = string_to_json(resp);
    if (!root.isObject()) {
        err = "response is not json";
        return false;
    }

    const std::string status = root.get("status", "").asString();
    const int retcode = root.get("retcode", 0).asInt();
    if (status == "ok" || retcode == 0) {
        return true;
    }

    err = fmt::format("status={} retcode={} msg={}", status, retcode,
                      root.get("message", root.get("msg", "")).asString());
    return false;
}

} // namespace biliget_utils
