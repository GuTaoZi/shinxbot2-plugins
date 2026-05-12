#include "bili_decode.h"

#include "bili_http.h"
#include "utils.h"

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

std::string compact_text(const std::string &raw, size_t max_len)
{
    std::string s = trim(raw);
    if (s.size() <= max_len) {
        return s;
    }
    return s.substr(0, max_len) + "...";
}

} // namespace

namespace biliget_decode {

std::string decode_video_text(const std::string &input_text)
{
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
        biliget_http::safe_get_json("https://api.bilibili.com", path);
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
    oss << "标题: " << compact_text(data.get("title", "").asString(), 120)
        << "\n";
    const std::string desc = data.get("desc", "").asString();
    if (!desc.empty()) {
        oss << "简介: " << compact_text(desc, 260) << "\n";
    }
    oss << "UP: " << data["owner"].get("name", "").asString() << "\n";
    oss << "播放 " << to_human_string(data["stat"].get("view", 0).asInt64())
        << " 点赞 " << to_human_string(data["stat"].get("like", 0).asInt64())
        << " 回复 " << to_human_string(data["stat"].get("reply", 0).asInt64())
        << " 弹幕 " << to_human_string(data["stat"].get("danmaku", 0).asInt64())
        << "\n";
    oss << "Link: https://www.bilibili.com/video/" << out_bvid << "/";
    return trim(oss.str());
}

} // namespace biliget_decode
