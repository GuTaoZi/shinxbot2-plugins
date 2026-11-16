#include "bili_decode.h"

#include "bili_http.h"
#include "utils.h"

#include <algorithm>
#include <curl/curl.h>
#include <regex>
#include <sstream>
#include <string>

namespace {

bool extract_bvid(const std::string &s, std::string &bvid)
{
    std::regex bv_re("BV[0-9A-Za-z]{10,}", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(s, m, bv_re)) {
        bvid = m.str(0);
        if (bvid.size() >= 2) {
            bvid[0] = 'B';
            bvid[1] = 'V';
        }
        return true;
    }
    return false;
}

bool extract_aid(const std::string &s, std::string &aid)
{
    std::regex av_re("\\bav([0-9]{1,20})\\b", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(s, m, av_re) && m.size() >= 2) {
        aid = m.str(1);
        return true;
    }
    return false;
}

// b23.tv / bili2233.cn short link, typed or JSON-escaped inside a shared card ("b23.tv\/QUdD66W").
bool extract_short_code(const std::string &s, std::string &host, std::string &code)
{
    static const std::regex re(R"((b23\.tv|bili2233\.cn)\\?/([0-9A-Za-z]{4,16}))",
                               std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_search(s, m, re)) {
        return false;
    }
    host = m.str(1);
    code = m.str(2);
    return true;
}

size_t discard_body(char *, size_t size, size_t nmemb, void *) { return size * nmemb; }

// Where the short link redirects to (one request, redirect not followed); "" on failure.
std::string resolve_short_link(const std::string &host, const std::string &code)
{
    CURL *c = curl_easy_init();
    if (!c) {
        return "";
    }
    const std::string url = "https://" + host + "/" + code;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/140 Safari/537.36");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_body);
    std::string target;
    if (curl_easy_perform(c) == CURLE_OK) {
        char *loc = nullptr;
        if (curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &loc) == CURLE_OK && loc) {
            target = loc;
        }
    }
    curl_easy_cleanup(c);
    return target;
}

// The text with a short link's target appended, when the text has no BV/av of its own.
std::string with_short_link_target(const std::string &text)
{
    static const std::regex bv_or_av(R"(BV[0-9A-Za-z]{10,}|\bav[0-9]{1,20}\b)", std::regex_constants::icase);
    std::string host, code;
    if (std::regex_search(text, bv_or_av) || !extract_short_code(text, host, code)) {
        return text;
    }
    const std::string target = resolve_short_link(host, code);
    return target.empty() ? text : text + " " + target;
}

// Truncate to at most max_len bytes without splitting a multi-byte UTF-8
// character in half (which renders as a mangled/incorrect trailing glyph).
std::string compact_text(const std::string &raw, size_t max_len)
{
    std::string s = trim(raw);
    if (s.size() <= max_len) {
        return s;
    }
    size_t cut = max_len;
    // Continuation bytes are 10xxxxxx; back off out of a character we'd
    // otherwise split apart from its leading byte.
    while (cut > 0 &&
           (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut) + "...";
}

// Escape CQ-code specials so untrusted text (video title/desc/uploader name
// are attacker-controlled) can't be misparsed as CQ code syntax, whether sent
// flat or wrapped into a forward node via string_to_messageArr.
std::string cq_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '[':
            o += "&#91;";
            break;
        case ']':
            o += "&#93;";
            break;
        default:
            o.push_back(c);
        }
    }
    return o;
}

} // namespace

namespace bili_decode {

std::string decode_video_text(const std::string &raw_text)
{
    const std::string input_text = with_short_link_target(raw_text);
    std::string bvid;
    std::string aid;

    if (!extract_bvid(input_text, bvid) && !extract_aid(input_text, aid)) {
        return "";
    }

    std::string path;
    if (!bvid.empty()) {
        path = "/x/web-interface/view?bvid=" + bvid;
    }
    else {
        path = "/x/web-interface/view?aid=" + aid;
    }

    Json::Value root =
        bili_http::safe_get_json("https://api.bilibili.com", path);
    if (!root.isObject() || root.get("code", -1).asInt() != 0 ||
        !root["data"].isObject()) {
        return "";
    }

    const Json::Value data = root["data"];
    const std::string out_bvid = data.get("bvid", "").asString();
    if (out_bvid.empty()) {
        return "";
    }

    std::ostringstream oss;
    const std::string pic = data.get("pic", "").asString();
    if (!pic.empty()) {
        oss << "[CQ:image,file=" << pic << ",id=40000]";
    }
    oss << out_bvid << " 分区: " << data.get("tname", "").asString() << "\n";
    oss << "标题: "
        << cq_escape(compact_text(data.get("title", "").asString(), 120))
        << "\n";
    const std::string desc = data.get("desc", "").asString();
    if (!desc.empty()) {
        oss << "简介: " << cq_escape(compact_text(desc, 260)) << "\n";
    }
    oss << "UP: " << cq_escape(data["owner"].get("name", "").asString())
        << "\n";
    oss << "播放 " << to_human_string(data["stat"].get("view", 0).asInt64())
        << " 点赞 " << to_human_string(data["stat"].get("like", 0).asInt64())
        << " 回复 " << to_human_string(data["stat"].get("reply", 0).asInt64())
        << " 弹幕 " << to_human_string(data["stat"].get("danmaku", 0).asInt64())
        << "\n";
    oss << "Link: https://www.bilibili.com/video/" << out_bvid << "/";
    return trim(oss.str());
}

bool fetch_video_info(const std::string &raw_text, bili_card::info_t &info)
{
    const std::string input_text = with_short_link_target(raw_text);
    std::string bvid;
    std::string aid;
    if (!extract_bvid(input_text, bvid) && !extract_aid(input_text, aid)) {
        return false;
    }
    const std::string path = !bvid.empty() ? "/x/web-interface/view?bvid=" + bvid
                                           : "/x/web-interface/view?aid=" + aid;
    const Json::Value root = bili_http::safe_get_json("https://api.bilibili.com", path);
    if (!root.isObject() || root.get("code", -1).asInt() != 0 || !root["data"].isObject()) {
        return false;
    }
    const Json::Value d = root["data"];
    const std::string out_bvid = d.get("bvid", "").asString();
    if (out_bvid.empty()) {
        return false;
    }
    const auto i64 = [](const Json::Value &v) -> int64_t {
        return v.isNumeric() ? v.asInt64() : -1;
    };
    info.kind = bili_card::kind_t::video_info;
    info.title = d.get("title", "").asString();
    info.up = d["owner"].get("name", "").asString();
    info.face_url = d["owner"].get("face", "").asString();
    info.cover_url = d.get("pic", "").asString();
    info.link = "https://www.bilibili.com/video/" + out_bvid;
    info.ts = i64(d["pubdate"]);
    info.duration_sec = static_cast<int>(std::max<int64_t>(0, i64(d["duration"])));
    info.category = d.get("tname", "").asString();
    info.desc = d.get("desc", "").asString();
    if (info.desc == "-") {
        info.desc.clear();
    }
    const Json::Value s = d["stat"];
    info.stats.view = i64(s["view"]);
    info.stats.danmaku = i64(s["danmaku"]);
    info.stats.like = i64(s["like"]);
    info.stats.coin = i64(s["coin"]);
    info.stats.favorite = i64(s["favorite"]);
    info.stats.share = i64(s["share"]);
    return true;
}

} // namespace bili_decode
