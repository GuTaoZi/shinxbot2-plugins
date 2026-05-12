#include "bili.h"
#include "bili_debug.h"
#include "bili_decode.h"
#include "bili_http.h"
#include "bili_utils.h"

#include <algorithm>
#include <ctime>
#include <fmt/format.h>
#include <regex>
#include <sstream>

namespace {
const std::string BILIGET_CONFIG_FILE =
    bot_config_path(nullptr, "features/biliget/biliget.json");
const std::string BILIGET_CACHE_FILE =
    bot_config_path(nullptr, "features/biliget/cache.json");
const std::string BILIGET_SECRET_FILE =
    bot_config_path(nullptr, "features/biliget/secret.json");
const std::string LEGACY_BILI_PUSH_SECRET_FILE =
    bot_config_path(nullptr, "features/bili_push/secret.json");
constexpr std::time_t BILIGET_LIST_NAME_CACHE_TTL_SEC = 1800;

using biliget_http::safe_get_json;

using biliget_utils::api_fail_reason;
using biliget_utils::compact_cover_url_169;
using biliget_utils::compact_cover_url_avatar;
using biliget_utils::compact_cover_url_keep_ratio;
using biliget_utils::compact_cover_url_square;
using biliget_utils::first_non_empty;
using biliget_utils::join_tokens;
using biliget_utils::json_to_i64;
using biliget_utils::live_jump_link;
using biliget_utils::mask_cookie_text;
using biliget_utils::send_group_msg_checked;
using biliget_utils::str_all_digits;
using biliget_utils::str_to_u64_if_digits;
using biliget_utils::up_name_with_uid;
using biliget_utils::url_encode;

std::string stringify_dynamic_type(const Json::Value &item)
{
    if (item.isMember("type") && item["type"].isString()) {
        return item["type"].asString();
    }
    return "DYNAMIC";
}

bool has_video_token_for_decode(const std::string &s)
{
    static const std::regex bv_re("BV[0-9A-Za-z]{10,}",
                                  std::regex_constants::icase);
    static const std::regex av_re("\\bav([0-9]{1,20})\\b",
                                  std::regex_constants::icase);
    return std::regex_search(s, bv_re) || std::regex_search(s, av_re);
}

help_level_t resolve_help_level(const msg_meta &conf)
{
    if (conf.p != nullptr && conf.p->is_op(conf.user_id)) {
        return help_level_t::bot_admin;
    }
    if (conf.message_type == "group" && conf.p != nullptr &&
        is_group_op(conf.p, conf.group_id, conf.user_id)) {
        return help_level_t::group_admin;
    }
    return help_level_t::public_only;
}

std::string build_help_text(help_level_t level, bool in_group,
                            bool member_manage_open)
{
    std::ostringstream oss;
    oss << "Biliget\n";
    oss << "----------\n";
    oss << "bili.help\n";
    oss << "bili.list\n";
    oss << "bili.live\n";
    oss << "bili.query <uid/房间号/用户名>\n";
    oss << "bili.debug <uid/房间号/用户名>\n";
    oss << "bili.wouldpush <uid/用户名>\n";
    oss << "bili.decode <BV/av/文本>\n";
    oss << "bili.import <bili.list文本>\n";
    oss << "bili.status";

    const bool can_member_manage = in_group && member_manage_open;
    const bool can_admin_manage = (level == help_level_t::group_admin ||
                                   level == help_level_t::bot_admin);

    if (can_member_manage) {
        oss << "\n---------- 群友可用 ----------\n";
        oss << "bili.add <uid/用户名...>\n";
        oss << "bili.del <uid/用户名...>\n";
        oss << "bili.set <uid/用户名...>\n";
        oss << "bili.clear";
    }

    if (can_admin_manage) {
        oss << "\n---------- 群管可用 ----------\n";
        oss << "bili.add <uid/用户名...>\n";
        oss << "bili.del <uid/用户名...>\n";
        oss << "bili.set <uid/用户名...>\n";
        oss << "bili.clear\n";
        oss << "bili.perm status|on|off\n";
    }

    if (level == help_level_t::bot_admin) {
        oss << "\n---------- OP ----------\n";
        oss << "bili.simulate <uid/用户名> <video|feed|live_on|live_off>\n";
        oss << "bili.pollnow\n";
        oss << "bili.cookie status|set <cookie>|clear|test <uid/用户名>\n";
        oss << "bili.interval <seconds>\n";
        oss << "bili.reload";
    }
    return oss.str();
}

bool is_pinned_video_item(const Json::Value &item)
{
    if (!item.isObject()) {
        return false;
    }
    if (item.get("is_top", 0).asInt() == 1 ||
        item.get("is_top", false).asBool()) {
        return true;
    }
    if (item.get("is_topped", 0).asInt() == 1 ||
        item.get("is_topped", false).asBool()) {
        return true;
    }
    const std::string tag = item.get("tag", "").asString();
    return tag.find("置顶") != std::string::npos;
}

bool is_pinned_dynamic_item(const Json::Value &item)
{
    if (!item.isObject()) {
        return false;
    }
    if (item.get("is_topped", 0).asInt() == 1 ||
        item.get("is_topped", false).asBool()) {
        return true;
    }
    const std::string tag_text =
        item["modules"]["module_tag"].get("text", "").asString();
    if (tag_text.find("置顶") != std::string::npos) {
        return true;
    }
    return item["modules"]["module_author"].get("is_top", 0).asInt() == 1;
}

Json::Value pick_latest_item(const Json::Value &items,
                             bool (*is_pinned)(const Json::Value &))
{
    if (!items.isArray() || items.empty()) {
        return Json::Value();
    }

    Json::Value first_any;
    Json::Value first_non_top;
    for (const auto &item : items) {
        if (!item.isObject()) {
            continue;
        }
        if (first_any.isNull()) {
            first_any = item;
        }
        if (!is_pinned(item)) {
            first_non_top = item;
            break;
        }
    }
    return first_non_top.isObject() ? first_non_top : first_any;
}

Json::Value pick_latest_dynamic_item_for_push(const Json::Value &items)
{
    if (!items.isArray() || items.empty()) {
        return Json::Value();
    }

    const auto better_item = [](const Json::Value &a,
                                const Json::Value &b) -> bool {
        const int64_t a_ts =
            json_to_i64(a["modules"]["module_author"].get("pub_ts", 0));
        const int64_t b_ts =
            json_to_i64(b["modules"]["module_author"].get("pub_ts", 0));
        if (a_ts != b_ts) {
            return a_ts > b_ts;
        }

        auto get_id_u64 = [](const Json::Value &it) -> uint64_t {
            std::string id = it.get("id_str", "").asString();
            if (id.empty() && it.isMember("id")) {
                id = std::to_string(it["id"].asUInt64());
            }
            if (!str_all_digits(id)) {
                return 0;
            }
            return static_cast<uint64_t>(std::stoull(id));
        };

        return get_id_u64(a) > get_id_u64(b);
    };

    Json::Value best_non_top;
    Json::Value best_any;
    for (const auto &item : items) {
        if (!item.isObject()) {
            continue;
        }
        const Json::Value major =
            item["modules"]["module_dynamic"].get("major", Json::Value());
        const std::string typ = item.get("type", "").asString();
        if (typ == "DYNAMIC_TYPE_LIVE_RCMD" ||
            (major.isObject() && major.isMember("live_rcmd"))) {
            continue;
        }

        if (!best_any.isObject() || better_item(item, best_any)) {
            best_any = item;
        }
        if (!is_pinned_dynamic_item(item)) {
            if (!best_non_top.isObject() || better_item(item, best_non_top)) {
                best_non_top = item;
            }
        }
    }
    return best_non_top.isObject() ? best_non_top : best_any;
}

std::string extract_dynamic_cover_from_modules(const Json::Value &item)
{
    const Json::Value major =
        item["modules"]["module_dynamic"].get("major", Json::Value());
    if (!major.isObject()) {
        return "";
    }

    return first_non_empty({
        major["draw"]["items"][0].get("src", "").asString(),
        major["archive"].get("cover", "").asString(),
        major["article"]["covers"][0].asString(),
        major["opus"]["pics"][0].get("url", "").asString(),
    });
}

std::string extract_dynamic_text(const Json::Value &item)
{
    std::string text =
        item["modules"]["module_dynamic"]["desc"].get("text", "").asString();
    if (!text.empty()) {
        return text;
    }

    const Json::Value major =
        item["modules"]["module_dynamic"].get("major", Json::Value());
    if (!major.isObject()) {
        return stringify_dynamic_type(item);
    }

    text = major["archive"].get("title", "").asString();
    if (!text.empty()) {
        return text;
    }

    text = major["article"].get("title", "").asString();
    if (!text.empty()) {
        return text;
    }

    text = major["opus"].get("title", "").asString();
    if (text.empty()) {
        text = major["opus"]["summary"].get("text", "").asString();
    }
    if (!text.empty()) {
        return text;
    }

    const std::string live_raw =
        major["live_rcmd"].get("content", "").asString();
    if (!live_raw.empty()) {
        Json::Value live_j = string_to_json(live_raw);
        text = live_j["live_play_info"].get("title", "").asString();
        if (!text.empty()) {
            return text;
        }
        text = live_j["live_play_info"].get("area_name", "").asString();
        if (!text.empty()) {
            return text;
        }
    }

    if (major["draw"]["items"].isArray() && !major["draw"]["items"].empty()) {
        return "图文动态";
    }

    return stringify_dynamic_type(item);
}

bool dynamic_cover_should_169(const Json::Value &item)
{
    const std::string typ = item.get("type", "").asString();
    if (typ == "DYNAMIC_TYPE_AV" || typ == "DYNAMIC_TYPE_LIVE_RCMD") {
        return true;
    }
    const Json::Value major =
        item["modules"]["module_dynamic"].get("major", Json::Value());
    if (!major.isObject()) {
        return false;
    }
    if (major.isMember("archive") || major.isMember("live_rcmd")) {
        return true;
    }
    return false;
}

std::string extract_live_cover_from_data(const Json::Value &data)
{
    if (!data.isObject()) {
        return "";
    }
    return first_non_empty({
        data.get("user_cover", "").asString(),
        data.get("cover_from_user", "").asString(),
        data.get("keyframe", "").asString(),
        data.get("cover", "").asString(),
        data["room_info"].get("cover", "").asString(),
    });
}

} // namespace

biliget::biliget()
{
    std::lock_guard<std::mutex> lock(mutex_);
    load_config_unlocked();
    load_cache_unlocked();
    load_cookie_secret_unlocked();
    next_poll_ts_ = std::time(nullptr) + 15;
}

biliget::~biliget()
{
    std::lock_guard<std::mutex> lock(mutex_);
    save_config_unlocked();
    save_cache_unlocked();
    save_cookie_secret_unlocked();
}

std::string biliget::config_path() const { return BILIGET_CONFIG_FILE; }

std::string biliget::cache_path() const { return BILIGET_CACHE_FILE; }

void biliget::load_cookie_secret_unlocked()
{
    cookie_override_.clear();
    {
        Json::Value root = string_to_json(readfile(BILIGET_SECRET_FILE, "{}"));
        if (root.isObject()) {
            cookie_override_ = root.get("cookie", "").asString();
        }
    }

    // Migration compatibility: when renamed from bili_push -> biliget,
    // reuse legacy cookie if new secret file is still empty.
    if (cookie_override_.empty()) {
        Json::Value legacy =
            string_to_json(readfile(LEGACY_BILI_PUSH_SECRET_FILE, "{}"));
        if (legacy.isObject()) {
            cookie_override_ = legacy.get("cookie", "").asString();
            if (!cookie_override_.empty()) {
                save_cookie_secret_unlocked();
            }
        }
    }

    if (!cookie_override_.empty()) {
        biliget_http::set_cookie_override(cookie_override_);
    }
    else {
        biliget_http::clear_cookie_override();
    }
}

void biliget::save_cookie_secret_unlocked() const
{
    Json::Value root(Json::objectValue);
    root["cookie"] = cookie_override_;
    writefile(BILIGET_SECRET_FILE, root.toStyledString(), false);
}

void biliget::load_config_unlocked()
{
    group_subscriptions_.clear();
    group_member_manage_open_.clear();

    Json::Value root = string_to_json(readfile(config_path(), "{}"));
    if (!root.isObject()) {
        poll_interval_sec_ = kDefaultPollIntervalSec;
        return;
    }

    poll_interval_sec_ = std::max(
        60, root.get("poll_interval_sec", kDefaultPollIntervalSec).asInt());

    const Json::Value groups = root["groups"];
    if (!groups.isObject()) {
        return;
    }

    for (const auto &gid_s : groups.getMemberNames()) {
        if (!str_all_digits(gid_s)) {
            continue;
        }
        groupid_t gid = static_cast<groupid_t>(std::stoull(gid_s));
        const Json::Value &arr = groups[gid_s];
        if (!arr.isArray()) {
            continue;
        }

        std::set<userid_t> st;
        for (const auto &one : arr) {
            if (one.isUInt64()) {
                st.insert(one.asUInt64());
            }
            else if (one.isString() && str_all_digits(one.asString())) {
                st.insert(static_cast<userid_t>(std::stoull(one.asString())));
            }
        }

        if (!st.empty()) {
            group_subscriptions_[gid] = std::move(st);
        }
    }

    const Json::Value open_groups = root["member_manage_open_groups"];
    if (open_groups.isArray()) {
        for (const auto &one : open_groups) {
            if (one.isUInt64()) {
                group_member_manage_open_.insert(one.asUInt64());
            }
            else if (one.isString() && str_all_digits(one.asString())) {
                group_member_manage_open_.insert(
                    static_cast<groupid_t>(std::stoull(one.asString())));
            }
        }
    }
}

void biliget::save_config_unlocked() const
{
    Json::Value root(Json::objectValue);
    root["poll_interval_sec"] = poll_interval_sec_;

    Json::Value groups(Json::objectValue);
    for (const auto &it : group_subscriptions_) {
        Json::Value arr(Json::arrayValue);
        for (userid_t uid : it.second) {
            arr.append(Json::UInt64(uid));
        }
        groups[std::to_string(it.first)] = arr;
    }
    root["groups"] = groups;

    Json::Value open_groups(Json::arrayValue);
    for (groupid_t gid : group_member_manage_open_) {
        open_groups.append(Json::UInt64(gid));
    }
    root["member_manage_open_groups"] = open_groups;

    writefile(config_path(), root.toStyledString(), false);
}

void biliget::load_cache_unlocked()
{
    up_cache_.clear();
    list_name_cache_.clear();

    Json::Value root = string_to_json(readfile(cache_path(), "{}"));
    if (!root.isObject() || !root["users"].isObject()) {
        return;
    }

    const Json::Value users = root["users"];
    for (const auto &uid_s : users.getMemberNames()) {
        if (!str_all_digits(uid_s)) {
            continue;
        }

        const Json::Value &u = users[uid_s];
        up_cache_t c;
        c.initialized = u.get("initialized", false).asBool();
        c.name = u.get("name", "").asString();
        c.last_dynamic_id = u.get("last_dynamic_id", "").asString();
        c.last_dynamic_text = u.get("last_dynamic_text", "").asString();
        c.last_dynamic_pub_ts = json_to_i64(u.get("last_dynamic_pub_ts", 0));
        c.last_video_bvid = u.get("last_video_bvid", "").asString();
        c.last_video_title = u.get("last_video_title", "").asString();
        c.last_video_pub_ts = json_to_i64(u.get("last_video_pub_ts", 0));
        c.last_live_status = u.get("last_live_status", -1).asInt();
        c.last_live_room_id = u.get("last_live_room_id", 0).asUInt64();
        c.last_live_title = u.get("last_live_title", "").asString();

        up_cache_[static_cast<userid_t>(std::stoull(uid_s))] = std::move(c);
    }
}

void biliget::save_cache_unlocked() const
{
    Json::Value root(Json::objectValue);
    Json::Value users(Json::objectValue);

    for (const auto &it : up_cache_) {
        Json::Value u(Json::objectValue);
        const auto &c = it.second;
        u["initialized"] = c.initialized;
        u["name"] = c.name;
        u["last_dynamic_id"] = c.last_dynamic_id;
        u["last_dynamic_text"] = c.last_dynamic_text;
        u["last_dynamic_pub_ts"] = Json::Int64(c.last_dynamic_pub_ts);
        u["last_video_bvid"] = c.last_video_bvid;
        u["last_video_title"] = c.last_video_title;
        u["last_video_pub_ts"] = Json::Int64(c.last_video_pub_ts);
        u["last_live_status"] = c.last_live_status;
        u["last_live_room_id"] = Json::UInt64(c.last_live_room_id);
        u["last_live_title"] = c.last_live_title;
        users[std::to_string(it.first)] = u;
    }

    root["users"] = users;
    writefile(cache_path(), root.toStyledString(), false);
}

bool biliget::is_uid_token(const std::string &token)
{
    return str_all_digits(trim(token));
}

bool biliget::resolve_target_to_uid(const std::string &token, userid_t &uid_out,
                                    std::string &err,
                                    bool allow_room_id_resolution) const
{
    uid_out = 0;
    const std::string t = trim(token);
    if (t.empty()) {
        err = "空参数";
        return false;
    }
    if (is_uid_token(t)) {
        uid_out = static_cast<userid_t>(std::stoull(t));

        if (allow_room_id_resolution) {
            // If numeric token has strong evidence as a UID, do not remap by
            // room.
            up_snapshot_t probe;
            const bool uid_looks_valid =
                fetch_profile_snapshot(uid_out, probe) ||
                fetch_live_snapshot(uid_out, probe) ||
                fetch_live_snapshot_via_master(uid_out, probe);
            if (!uid_looks_valid) {
                userid_t resolved_uid = 0;
                up_snapshot_t room_probe;
                if (resolve_uid_by_room_id(uid_out, room_probe, resolved_uid) &&
                    resolved_uid != 0) {
                    uid_out = resolved_uid;
                }
            }
        }
        return true;
    }

    const auto parse_user_list = [&](const Json::Value &arr, userid_t &exact,
                                     userid_t &first) {
        if (!arr.isArray()) {
            return;
        }
        for (const auto &one : arr) {
            if (!one.isObject()) {
                continue;
            }
            userid_t mid = 0;
            if (one["mid"].isString() &&
                str_all_digits(one["mid"].asString())) {
                mid = static_cast<userid_t>(std::stoull(one["mid"].asString()));
            }
            else if (one["mid"].isUInt64()) {
                mid = one["mid"].asUInt64();
            }
            if (mid == 0) {
                continue;
            }
            if (first == 0) {
                first = mid;
            }
            const std::string uname = trim(one.get("uname", "").asString());
            if (!uname.empty() && uname == t) {
                exact = mid;
                return;
            }
        }
    };

    userid_t exact = 0;
    userid_t first = 0;

    const std::string path1 =
        "/x/web-interface/search/type?search_type=bili_user&page=1&keyword=" +
        url_encode(t);
    Json::Value root1 = safe_get_json("https://api.bilibili.com", path1);
    if (root1.isObject() && root1.get("code", -1).asInt() == 0) {
        parse_user_list(root1["data"]["result"], exact, first);
        if (exact != 0 || first != 0) {
            uid_out = (exact != 0) ? exact : first;
            return true;
        }
    }

    const std::string path2 =
        "/x/web-interface/search/all/v2?keyword=" + url_encode(t);
    Json::Value root2 = safe_get_json("https://api.bilibili.com", path2);
    if (root2.isObject() && root2.get("code", -1).asInt() == 0) {
        const Json::Value result = root2["data"]["result"];
        if (result.isArray()) {
            for (const auto &block : result) {
                if (!block.isObject()) {
                    continue;
                }
                if (block.get("result_type", "").asString() != "bili_user") {
                    continue;
                }
                parse_user_list(block["data"], exact, first);
                break;
            }
        }
        if (exact != 0 || first != 0) {
            uid_out = (exact != 0) ? exact : first;
            return true;
        }
    }

    err = "未找到该用户名，或搜索接口被风控";
    return false;
}

std::vector<userid_t>
biliget::parse_uid_list(const std::string &args,
                        std::vector<std::string> *failed_tokens) const
{
    std::string normalized = args;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::replace(normalized.begin(), normalized.end(), ';', ' ');
    normalized = regex_replace(normalized, std::regex("，|；"), " ");

    std::istringstream iss(normalized);
    std::set<userid_t> uniq;
    std::string token;
    while (iss >> token) {
        token = trim(token);
        userid_t uid = 0;
        std::string err;
        if (!resolve_target_to_uid(token, uid, err)) {
            if (failed_tokens != nullptr) {
                failed_tokens->push_back(token);
            }
            continue;
        }
        uniq.insert(uid);
    }

    return std::vector<userid_t>(uniq.begin(), uniq.end());
}

bool biliget::can_manage_group(const msg_meta &conf) const
{
    if (conf.p == nullptr) {
        return false;
    }
    if (conf.p->is_op(conf.user_id)) {
        return true;
    }
    if (conf.message_type == "group") {
        return is_group_op(conf.p, conf.group_id, conf.user_id);
    }
    return false;
}

bool biliget::is_group_member_manage_open(groupid_t gid) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return group_member_manage_open_.count(gid) > 0;
}

bool biliget::can_modify_subscriptions(const msg_meta &conf) const
{
    if (can_manage_group(conf)) {
        return true;
    }
    if (conf.message_type != "group") {
        return false;
    }
    return is_group_member_manage_open(conf.group_id);
}

std::string biliget::build_help_for_context(const msg_meta &conf) const
{
    const help_level_t level = resolve_help_level(conf);
    const bool in_group = (conf.message_type == "group");
    const bool open =
        in_group ? is_group_member_manage_open(conf.group_id) : false;
    return build_help_text(level, in_group, open);
}

bool biliget::parse_group_and_uids(
    const msg_meta &conf, const std::string &args, groupid_t &target_group,
    std::vector<userid_t> &uids, bool allow_empty_uid_list,
    std::vector<std::string> *failed_tokens) const
{
    std::string body = trim(args);
    if (conf.message_type != "group") {
        return false;
    }
    target_group = conf.group_id;
    uids = parse_uid_list(body, failed_tokens);
    return allow_empty_uid_list || !uids.empty();
}

std::string biliget::compact_text(const std::string &raw, size_t max_len)
{
    std::string s = trim(raw);
    if (s.empty()) {
        return s;
    }
    if (s.size() <= max_len) {
        return s;
    }
    return s.substr(0, max_len) + "...";
}

bool biliget::fetch_live_snapshot(userid_t uid, up_snapshot_t &snapshot) const
{
    const std::string path =
        "/room/v1/Room/get_status_info_by_uids?uids%5B%5D=" +
        std::to_string(uid);
    Json::Value root = safe_get_json("https://api.live.bilibili.com", path);
    if (!root.isObject() || root.get("code", -1).asInt() != 0 ||
        (!root["data"].isObject() && !root["data"].isArray())) {
        return false;
    }

    Json::Value one;
    const Json::Value data = root["data"];
    if (data.isObject()) {
        one = data[std::to_string(uid)];
    }
    else if (data.isArray()) {
        for (const auto &entry : data) {
            if (!entry.isObject()) {
                continue;
            }
            if (entry.get("uid", 0).asUInt64() == uid ||
                entry.get("room_id", 0).asUInt64() == uid) {
                one = entry;
                break;
            }
        }
    }

    if (!one.isObject()) {
        return false;
    }

    snapshot.has_any = true;
    snapshot.has_live = true;
    snapshot.live_status = one.get("live_status", -1).asInt();
    snapshot.live_room_id = one.get("room_id", 0).asUInt64();
    snapshot.live_title = one.get("title", "").asString();
    if (snapshot.live_cover.empty()) {
        snapshot.live_cover =
            compact_cover_url_169(extract_live_cover_from_data(one));
    }
    if (snapshot.canonical_uid == 0) {
        snapshot.canonical_uid = one.get("uid", uid).asUInt64();
    }

    if (snapshot.name.empty()) {
        snapshot.name = one.get("uname", "").asString();
    }
    if (snapshot.avatar.empty()) {
        snapshot.avatar =
            compact_cover_url_avatar(one.get("face", "").asString());
    }

    return true;
}

bool biliget::resolve_uid_by_room_id(userid_t room_id, up_snapshot_t &snapshot,
                                     userid_t &uid_out) const
{
    uid_out = 0;
    const std::vector<std::string> paths = {
        "/xlive/web-room/v1/index/getRoomPlayInfo?room_id=" +
            std::to_string(room_id),
        "/room/v1/Room/room_init?id=" + std::to_string(room_id),
        "/room/v1/Room/get_info?room_id=" + std::to_string(room_id),
    };

    bool found = false;
    for (const auto &path : paths) {
        Json::Value root = safe_get_json("https://api.live.bilibili.com", path);
        if (!root.isObject() || root.get("code", -1).asInt() != 0 ||
            !root["data"].isObject()) {
            continue;
        }

        const Json::Value data = root["data"];
        const userid_t cand_uid = data.get("uid", 0).asUInt64();
        if (cand_uid == 0) {
            continue;
        }

        if (uid_out == 0) {
            uid_out = cand_uid;
        }

        snapshot.has_any = true;
        snapshot.has_live = true;
        if (snapshot.live_status < 0) {
            snapshot.live_status = data.get("live_status", -1).asInt();
        }

        const userid_t rid = data.get("room_id", room_id).asUInt64();
        if (snapshot.live_room_id == 0 && rid != 0) {
            snapshot.live_room_id = rid;
        }

        const std::string title = data.get("title", "").asString();
        if (snapshot.live_title.empty() && !title.empty()) {
            snapshot.live_title = title;
        }
        if (snapshot.live_cover.empty()) {
            snapshot.live_cover =
                compact_cover_url_169(extract_live_cover_from_data(data));
        }

        if (snapshot.canonical_uid == 0) {
            snapshot.canonical_uid = uid_out;
        }
        if (snapshot.name.empty()) {
            snapshot.name = data.get("uname", "").asString();
        }
        if (snapshot.avatar.empty()) {
            snapshot.avatar =
                compact_cover_url_avatar(data.get("face", "").asString());
        }

        found = true;
    }

    return found;
}

bool biliget::fetch_live_snapshot_via_master(userid_t uid,
                                             up_snapshot_t &snapshot) const
{
    const std::string path =
        "/live_user/v1/Master/info?uid=" + std::to_string(uid);
    Json::Value root = safe_get_json("https://api.live.bilibili.com", path);
    if (!root.isObject() || root.get("code", -1).asInt() != 0 ||
        !root["data"].isObject()) {
        return false;
    }

    const Json::Value data = root["data"];
    const userid_t room_id = data.get("room_id", 0).asUInt64();
    if (room_id == 0) {
        return false;
    }

    if (snapshot.name.empty()) {
        snapshot.name = data["info"].get("uname", "").asString();
    }
    if (snapshot.avatar.empty()) {
        snapshot.avatar =
            compact_cover_url_avatar(data["info"].get("face", "").asString());
    }
    snapshot.canonical_uid = uid;
    snapshot.has_any = true;
    snapshot.live_room_id = room_id;

    userid_t resolved_uid = 0;
    if (resolve_uid_by_room_id(room_id, snapshot, resolved_uid)) {
        if (resolved_uid != 0) {
            snapshot.canonical_uid = resolved_uid;
        }

        // If room lookup succeeded, it already filled live status/title.
        return true;
    }

    // Fallback: room->uid detail may fail transiently, still try uid status.
    if (!snapshot.has_live) {
        (void)fetch_live_snapshot(uid, snapshot);
    }

    // Even without definitive status, this path still provides useful room
    // info.
    return snapshot.live_room_id != 0;
}

bool biliget::fetch_profile_snapshot(userid_t uid,
                                     up_snapshot_t &snapshot) const
{
    const std::string path = "/x/web-interface/card?mid=" + std::to_string(uid);
    Json::Value root = safe_get_json("https://api.bilibili.com", path);
    if (!root.isObject() || root.get("code", -1).asInt() != 0 ||
        !root["data"].isObject()) {
        return false;
    }

    const Json::Value data = root["data"];
    snapshot.has_profile = true;
    snapshot.profile_archive_count =
        json_to_i64(data.get("archive_count", -1), -1);
    snapshot.profile_follower = json_to_i64(data["card"].get("fans", -1), -1);
    if (snapshot.name.empty()) {
        snapshot.name = data["card"].get("name", "").asString();
    }
    if (snapshot.avatar.empty()) {
        snapshot.avatar =
            compact_cover_url_avatar(data["card"].get("face", "").asString());
    }
    snapshot.has_any = true;
    return true;
}

bool biliget::fetch_video_snapshot(userid_t uid, up_snapshot_t &snapshot) const
{
    auto apply_video_from_dynamic_items =
        [&](const Json::Value &items, const std::string &api_name) -> bool {
        if (!items.isArray() || items.empty()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason += api_name + " 返回空 items";
            return false;
        }

        const auto better_video_item = [](const Json::Value &a,
                                          const Json::Value &b) -> bool {
            const int64_t a_ts =
                json_to_i64(a["modules"]["module_author"].get("pub_ts", 0));
            const int64_t b_ts =
                json_to_i64(b["modules"]["module_author"].get("pub_ts", 0));
            if (a_ts != b_ts) {
                return a_ts > b_ts;
            }

            auto get_id_u64 = [](const Json::Value &it) -> uint64_t {
                std::string id = it.get("id_str", "").asString();
                if (id.empty() && it.isMember("id")) {
                    id = std::to_string(it["id"].asUInt64());
                }
                if (!str_all_digits(id)) {
                    return 0;
                }
                return static_cast<uint64_t>(std::stoull(id));
            };
            return get_id_u64(a) > get_id_u64(b);
        };

        Json::Value first_any_video;
        Json::Value first_non_top_video;
        for (const auto &item : items) {
            if (!item.isObject()) {
                continue;
            }
            const Json::Value archive =
                item["modules"]["module_dynamic"]["major"]["archive"];
            if (!archive.isObject()) {
                continue;
            }
            if (archive.get("bvid", "").asString().empty()) {
                continue;
            }
            if (first_any_video.isNull() ||
                better_video_item(item, first_any_video)) {
                first_any_video = item;
            }
            if (!is_pinned_dynamic_item(item) &&
                (first_non_top_video.isNull() ||
                 better_video_item(item, first_non_top_video))) {
                first_non_top_video = item;
            }
        }

        const Json::Value pick = first_non_top_video.isObject()
                                     ? first_non_top_video
                                     : first_any_video;
        if (!pick.isObject()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason += api_name + " 未发现视频动态";
            return false;
        }

        const Json::Value archive =
            pick["modules"]["module_dynamic"]["major"]["archive"];
        if (!archive.isObject()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason += api_name + " 首项不含视频";
            return false;
        }

        const std::string bvid = archive.get("bvid", "").asString();
        if (bvid.empty()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason += api_name + " 首项无 bvid";
            return false;
        }

        snapshot.has_any = true;
        snapshot.has_video = true;
        snapshot.video_bvid = bvid;
        snapshot.video_title = archive.get("title", "").asString();
        snapshot.video_cover =
            compact_cover_url_169(archive.get("cover", "").asString());
        snapshot.video_pub_ts =
            json_to_i64(pick["modules"]["module_author"].get("pub_ts", 0));
        if (snapshot.name.empty()) {
            snapshot.name =
                pick["modules"]["module_author"].get("name", "").asString();
        }
        snapshot.video_reason.clear();
        return true;
    };

    const std::string path = "/x/space/arc/search?mid=" + std::to_string(uid) +
                             "&pn=1&ps=5&order=pubdate";
    {
        Json::Value root = safe_get_json("https://api.bilibili.com", path);
        snapshot.video_reason = api_fail_reason(root, "space.arc.search");
        if (root.isObject() && root.get("code", -1).asInt() == 0) {
            const Json::Value vlist = root["data"]["list"]["vlist"];
            if (vlist.isArray() && !vlist.empty()) {
                Json::Value first =
                    pick_latest_item(vlist, is_pinned_video_item);
                if (!first.isObject()) {
                    snapshot.video_reason = "space.arc.search 返回空列表";
                    return false;
                }
                std::string bvid = first.get("bvid", "").asString();
                if (bvid.empty() && first.isMember("aid")) {
                    bvid = "av" + std::to_string(first["aid"].asUInt64());
                }
                if (!bvid.empty()) {
                    snapshot.has_any = true;
                    snapshot.has_video = true;
                    snapshot.video_bvid = bvid;
                    snapshot.video_title = first.get("title", "").asString();
                    snapshot.video_cover =
                        compact_cover_url_169(first.get("pic", "").asString());
                    snapshot.video_pub_ts =
                        json_to_i64(first.get("created", 0));

                    if (snapshot.name.empty()) {
                        snapshot.name = first.get("author", "").asString();
                    }
                    snapshot.video_reason.clear();
                    return true;
                }
            }
            snapshot.video_reason = "space.arc.search 返回空列表";
        }
    }

    // Fallback: some accounts are intermittently blocked on arc/search, but
    // latest video can still be discovered from dynamic major.archive.
    {
        const std::string dyn_path =
            "/x/polymer/web-dynamic/v1/feed/space?host_mid=" +
            std::to_string(uid);
        Json::Value root = safe_get_json("https://api.bilibili.com", dyn_path);
        if (!root.isObject() || root.get("code", -1).asInt() != 0) {
            const std::string d = api_fail_reason(root, "dynamic.feed.space");
            if (!d.empty()) {
                if (!snapshot.video_reason.empty()) {
                    snapshot.video_reason += "；";
                }
                snapshot.video_reason += d;
            }
            // Keep going: dynamic.feed.all may still be available.
        }

        if (root.isObject() && root.get("code", -1).asInt() == 0) {
            const Json::Value items = root["data"]["items"];
            if (apply_video_from_dynamic_items(items, "dynamic.feed.space")) {
                return true;
            }
        }
    }

    {
        const std::string dyn_path =
            "/x/polymer/web-dynamic/v1/feed/all?type=all&host_mid=" +
            std::to_string(uid);
        Json::Value root = safe_get_json("https://api.bilibili.com", dyn_path);
        const std::string all_reason =
            api_fail_reason(root, "dynamic.feed.all");
        if (!all_reason.empty()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason += all_reason;
            return false;
        }
        const Json::Value items = root["data"]["items"];
        if (apply_video_from_dynamic_items(items, "dynamic.feed.all")) {
            return true;
        }
    }

    {
        const std::string dsvr_path = "/dynamic_svr/v1/dynamic_svr/"
                                      "space_history?visitor_uid=0&host_uid=" +
                                      std::to_string(uid) +
                                      "&offset_dynamic_id=0&need_top=1";
        Json::Value root =
            safe_get_json("https://api.vc.bilibili.com", dsvr_path);
        if (!root.isObject() || root.get("code", -1).asInt() != 0) {
            const std::string d =
                api_fail_reason(root, "dynamic_svr.space_history.video");
            if (!d.empty()) {
                if (!snapshot.video_reason.empty()) {
                    snapshot.video_reason += "；";
                }
                snapshot.video_reason += d;
            }
            return false;
        }

        const Json::Value cards = root["data"]["cards"];
        if (!cards.isArray() || cards.empty()) {
            if (!snapshot.video_reason.empty()) {
                snapshot.video_reason += "；";
            }
            snapshot.video_reason +=
                "dynamic_svr.space_history.video cards 为空";
            return false;
        }

        for (const auto &one : cards) {
            if (!one.isObject()) {
                continue;
            }

            std::string bvid;
            std::string title;
            std::string cover;
            uint64_t aid = 0;

            const std::string card_raw = one.get("card", "").asString();
            Json::Value card = string_to_json(card_raw);
            if (card.isObject()) {
                bvid = card.get("bvid", "").asString();
                title = card.get("title", "").asString();
                cover = card.get("pic", "").asString();
                if (card["aid"].isUInt64()) {
                    aid = card["aid"].asUInt64();
                }
                else if (card["aid"].isString() &&
                         str_all_digits(card["aid"].asString())) {
                    aid = static_cast<uint64_t>(
                        std::stoull(card["aid"].asString()));
                }
            }

            if (bvid.empty() && aid != 0) {
                Json::Value v = safe_get_json("https://api.bilibili.com",
                                              "/x/web-interface/view?aid=" +
                                                  std::to_string(aid));
                if (v.isObject() && v.get("code", -1).asInt() == 0) {
                    bvid = v["data"].get("bvid", "").asString();
                    if (title.empty()) {
                        title = v["data"].get("title", "").asString();
                    }
                    if (cover.empty()) {
                        cover = v["data"].get("pic", "").asString();
                    }
                    if (snapshot.name.empty()) {
                        snapshot.name =
                            v["data"]["owner"].get("name", "").asString();
                    }
                }
            }

            if (bvid.empty()) {
                continue;
            }

            snapshot.has_any = true;
            snapshot.has_video = true;
            snapshot.video_bvid = bvid;
            snapshot.video_title = title;
            snapshot.video_cover = compact_cover_url_169(cover);
            snapshot.video_pub_ts =
                json_to_i64(one["desc"].get("timestamp", 0));

            if (snapshot.name.empty()) {
                snapshot.name = one["desc"]["user_profile"]["info"]
                                    .get("uname", "")
                                    .asString();
            }
            snapshot.video_reason.clear();
            return true;
        }

        if (!snapshot.video_reason.empty()) {
            snapshot.video_reason += "；";
        }
        snapshot.video_reason += "dynamic_svr.space_history.video 未发现视频";
    }

    return false;
}

bool biliget::fetch_dynamic_snapshot(userid_t uid,
                                     up_snapshot_t &snapshot) const
{
    auto apply_dynamic_item = [&](const Json::Value &item) -> bool {
        if (!item.isObject()) {
            return false;
        }
        if (item.get("type", "").asString() == "DYNAMIC_TYPE_LIVE_RCMD") {
            return false;
        }
        std::string id = item.get("id_str", "").asString();
        if (id.empty() && item.isMember("id")) {
            id = std::to_string(item["id"].asUInt64());
        }
        if (id.empty()) {
            return false;
        }

        snapshot.has_any = true;
        snapshot.has_dynamic = true;
        snapshot.dynamic_id = id;

        snapshot.dynamic_text = compact_text(extract_dynamic_text(item), 120);
        const std::string raw_cover = extract_dynamic_cover_from_modules(item);
        snapshot.dynamic_cover = dynamic_cover_should_169(item)
                                     ? compact_cover_url_169(raw_cover)
                                     : compact_cover_url_keep_ratio(raw_cover);
        snapshot.dynamic_pub_ts =
            json_to_i64(item["modules"]["module_author"].get("pub_ts", 0));

        std::string name = item["modules"]["module_author"]["name"].asString();
        if (!name.empty()) {
            snapshot.name = name;
        }
        snapshot.dynamic_reason.clear();
        return true;
    };

    {
        const std::string path =
            "/x/polymer/web-dynamic/v1/feed/all?type=all&host_mid=" +
            std::to_string(uid);
        Json::Value root = safe_get_json("https://api.bilibili.com", path);
        snapshot.dynamic_reason = api_fail_reason(root, "dynamic.feed.all");
        if (root.isObject() && root.get("code", -1).asInt() == 0) {
            const Json::Value items = root["data"]["items"];
            if (apply_dynamic_item(pick_latest_dynamic_item_for_push(items))) {
                return true;
            }
            snapshot.dynamic_reason = "dynamic.feed.all 返回空 items";
        }
    }

    {
        const std::string path =
            "/x/polymer/web-dynamic/v1/feed/space?host_mid=" +
            std::to_string(uid);
        Json::Value root = safe_get_json("https://api.bilibili.com", path);
        const std::string space_reason =
            api_fail_reason(root, "dynamic.feed.space");
        if (!space_reason.empty()) {
            if (!snapshot.dynamic_reason.empty()) {
                snapshot.dynamic_reason += "；";
            }
            snapshot.dynamic_reason += space_reason;
        }
        if (root.isObject() && root.get("code", -1).asInt() == 0) {
            const Json::Value items = root["data"]["items"];
            if (apply_dynamic_item(pick_latest_dynamic_item_for_push(items))) {
                return true;
            }
            if (!snapshot.dynamic_reason.empty()) {
                snapshot.dynamic_reason += "；";
            }
            snapshot.dynamic_reason += "dynamic.feed.space 返回空 items";
        }
    }

    {
        const std::string path = "/dynamic_svr/v1/dynamic_svr/"
                                 "space_history?visitor_uid=0&host_uid=" +
                                 std::to_string(uid) +
                                 "&offset_dynamic_id=0&need_top=1";
        Json::Value root = safe_get_json("https://api.vc.bilibili.com", path);
        if (!root.isObject() || root.get("code", -1).asInt() != 0) {
            const std::string d =
                api_fail_reason(root, "dynamic_svr.space_history");
            if (!d.empty()) {
                if (!snapshot.dynamic_reason.empty()) {
                    snapshot.dynamic_reason += "；";
                }
                snapshot.dynamic_reason += d;
            }
            return false;
        }

        const Json::Value cards = root["data"]["cards"];
        if (!cards.isArray() || cards.empty()) {
            if (!snapshot.dynamic_reason.empty()) {
                snapshot.dynamic_reason += "；";
            }
            snapshot.dynamic_reason += "dynamic_svr.space_history cards 为空";
            return false;
        }

        Json::Value picked_card;
        Json::Value picked_desc;
        Json::Value first_non_live;
        Json::Value first_non_live_desc;
        for (const auto &card : cards) {
            if (!card.isObject()) {
                continue;
            }
            const Json::Value desc = card["desc"];
            if (!desc.isObject()) {
                continue;
            }
            const int typ = desc.get("type", 0).asInt();
            if (typ == 4308) {
                continue;
            }
            const auto better_desc = [&](const Json::Value &a,
                                         const Json::Value &b) -> bool {
                const int64_t a_ts = json_to_i64(a.get("timestamp", 0));
                const int64_t b_ts = json_to_i64(b.get("timestamp", 0));
                if (a_ts != b_ts) {
                    return a_ts > b_ts;
                }
                std::string a_id = a.get("dynamic_id_str", "").asString();
                if (a_id.empty() && a.isMember("dynamic_id")) {
                    a_id = std::to_string(a["dynamic_id"].asUInt64());
                }
                std::string b_id = b.get("dynamic_id_str", "").asString();
                if (b_id.empty() && b.isMember("dynamic_id")) {
                    b_id = std::to_string(b["dynamic_id"].asUInt64());
                }
                uint64_t a_num = 0;
                uint64_t b_num = 0;
                if (str_to_u64_if_digits(a_id, a_num) &&
                    str_to_u64_if_digits(b_id, b_num)) {
                    return a_num > b_num;
                }
                return a_id > b_id;
            };

            if (first_non_live.isNull() ||
                better_desc(desc, first_non_live_desc)) {
                first_non_live = card;
                first_non_live_desc = desc;
            }
            const bool is_top = desc.get("is_top", 0).asInt() == 1 ||
                                desc.get("is_top", false).asBool();
            if (!is_top &&
                (!picked_card.isObject() || better_desc(desc, picked_desc))) {
                picked_card = card;
                picked_desc = desc;
            }
        }
        if (!picked_card.isObject()) {
            picked_card = first_non_live;
            picked_desc = first_non_live_desc;
        }
        if (!picked_card.isObject() || !picked_desc.isObject()) {
            return false;
        }

        std::string dyn_id;
        if (picked_desc.isMember("dynamic_id_str") &&
            picked_desc["dynamic_id_str"].isString()) {
            dyn_id = picked_desc["dynamic_id_str"].asString();
        }
        if (dyn_id.empty() && picked_desc.isMember("dynamic_id")) {
            dyn_id = std::to_string(picked_desc["dynamic_id"].asUInt64());
        }
        if (dyn_id.empty()) {
            return false;
        }

        snapshot.has_any = true;
        snapshot.has_dynamic = true;
        snapshot.dynamic_id = dyn_id;
        snapshot.dynamic_pub_ts = json_to_i64(picked_desc.get("timestamp", 0));

        std::string text;
        const std::string card_raw = picked_card.get("card", "").asString();
        Json::Value card = string_to_json(card_raw);
        if (card.isObject()) {
            text = card["item"]["description"].asString();
            if (text.empty()) {
                text = card["title"].asString();
            }
            snapshot.dynamic_cover = first_non_empty({
                card.get("pic", "").asString(),
                card["item"].get("pic", "").asString(),
                card["item"]["pictures"][0].get("img_src", "").asString(),
            });
        }
        if (text.empty()) {
            text = "dynamic type " +
                   std::to_string(picked_desc.get("type", 0).asInt());
        }
        snapshot.dynamic_text = compact_text(text, 120);
        snapshot.dynamic_cover =
            compact_cover_url_keep_ratio(snapshot.dynamic_cover);

        if (snapshot.name.empty()) {
            snapshot.name = picked_card["desc"]["user_profile"]["info"]
                                .get("uname", "")
                                .asString();
        }
        snapshot.dynamic_reason.clear();
        return true;
    }

    return false;
}

biliget::up_snapshot_t biliget::fetch_snapshot(userid_t uid) const
{
    up_snapshot_t s;
    userid_t target_uid = uid;

    if (!s.has_live) {
        (void)fetch_live_snapshot(target_uid, s);
    }
    if (!s.has_live) {
        (void)fetch_live_snapshot_via_master(target_uid, s);
    }

    if (s.canonical_uid != 0) {
        target_uid = s.canonical_uid;
    }

    (void)fetch_profile_snapshot(target_uid, s);
    (void)fetch_video_snapshot(target_uid, s);
    (void)fetch_dynamic_snapshot(target_uid, s);

    if (s.canonical_uid == 0) {
        s.canonical_uid = target_uid;
    }
    return s;
}

biliget::up_snapshot_t biliget::fetch_snapshot_for_poll(userid_t uid) const
{
    up_snapshot_t s;
    userid_t target_uid = uid;

    if (!s.has_live) {
        (void)fetch_live_snapshot(target_uid, s);
    }
    if (!s.has_live) {
        (void)fetch_live_snapshot_via_master(target_uid, s);
    }

    if (s.canonical_uid != 0) {
        target_uid = s.canonical_uid;
    }

    (void)fetch_video_snapshot(target_uid, s);
    (void)fetch_dynamic_snapshot(target_uid, s);

    if (s.canonical_uid == 0) {
        s.canonical_uid = target_uid;
    }

    if (s.name.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = up_cache_.find(uid);
        if (it != up_cache_.end()) {
            s.name = it->second.name;
        }
    }
    return s;
}

std::string biliget::build_dynamic_push_message(userid_t uid,
                                                const up_snapshot_t &snap) const
{
    std::ostringstream oss;
    if (!snap.dynamic_cover.empty()) {
        oss << "[CQ:image,file=" << snap.dynamic_cover << ",id=40000]";
    }
    oss << "你关注的" << (snap.name.empty() ? std::to_string(uid) : snap.name)
        << "发新动态啦！\n";
    oss << "UP: " << up_name_with_uid(uid, snap.name) << "\n";
    if (!snap.dynamic_text.empty()) {
        oss << compact_text(snap.dynamic_text, 180) << "\n";
    }
    oss << "https://t.bilibili.com/" << snap.dynamic_id;
    return oss.str();
}

std::string biliget::build_video_push_message(userid_t uid,
                                              const up_snapshot_t &snap) const
{
    std::ostringstream oss;
    if (!snap.video_cover.empty()) {
        oss << "[CQ:image,file=" << snap.video_cover << ",id=40000]";
    }
    oss << "你关注的" << (snap.name.empty() ? std::to_string(uid) : snap.name)
        << "发布新视频啦！\n";
    oss << "UP: " << up_name_with_uid(uid, snap.name) << "\n";
    if (!snap.video_title.empty()) {
        oss << compact_text(snap.video_title, 180) << "\n";
    }
    oss << "https://www.bilibili.com/video/" << snap.video_bvid;
    return oss.str();
}

std::string biliget::build_live_on_push_message(userid_t uid,
                                                const up_snapshot_t &snap) const
{
    std::ostringstream oss;
    if (!snap.live_cover.empty()) {
        oss << "[CQ:image,file=" << snap.live_cover << ",id=40000]";
    }
    oss << "你关注的" << (snap.name.empty() ? std::to_string(uid) : snap.name)
        << "开播啦！\n";
    oss << "UP: " << up_name_with_uid(uid, snap.name) << "\n";
    if (!snap.live_title.empty()) {
        oss << compact_text(snap.live_title, 180) << "\n";
    }
    oss << live_jump_link(uid, snap.live_room_id);
    return oss.str();
}

std::string
biliget::build_live_off_push_message(userid_t uid,
                                     const up_snapshot_t &snap) const
{
    std::ostringstream oss;
    oss << "你关注的" << (snap.name.empty() ? std::to_string(uid) : snap.name)
        << "刚刚下播啦。\n";
    oss << "UP: " << up_name_with_uid(uid, snap.name) << "\n";
    if (!snap.live_title.empty()) {
        oss << compact_text(snap.live_title, 180) << "\n";
    }
    oss << live_jump_link(uid, snap.live_room_id);
    return oss.str();
}

std::string biliget::query_one(userid_t uid) const
{
    const up_snapshot_t snap = fetch_snapshot(uid);

    std::ostringstream oss;
    oss << "[Biliget 查询]\n";
    if (!snap.name.empty()) {
        oss << "UP: " << snap.name << "\n";
    }
    if (snap.has_profile && snap.profile_follower >= 0) {
        oss << "粉丝数：" << snap.profile_follower << "\n";
    }
    oss << "UID " << uid;
    if (snap.canonical_uid != 0 && snap.canonical_uid != uid) {
        oss << " -> " << snap.canonical_uid;
    }
    if (snap.live_room_id != 0) {
        oss << " / 房间号 " << snap.live_room_id;
    }
    oss << "\n";
    if (!snap.avatar.empty()) {
        oss << "[CQ:image,file=" << snap.avatar << ",id=40000]";
    }

    if (snap.has_live) {
        oss << (snap.live_status == 1 ? "直播中" : "未开播");
        if (!snap.live_title.empty()) {
            oss << ": " << compact_text(snap.live_title, 120);
        }
        oss << "\n";
        if (snap.live_room_id != 0) {
            oss << "https://live.bilibili.com/" << snap.live_room_id << "\n";
        }
        if (!snap.live_cover.empty()) {
            oss << "[CQ:image,file=" << snap.live_cover << ",id=40000]";
        }
    }
    else {
        oss << "未获取到直播信息\n";
    }

    if (snap.has_video) {
        oss << "最新视频：" << compact_text(snap.video_title, 120) << "\n";
        oss << "https://www.bilibili.com/video/" << snap.video_bvid << "\n";
        if (!snap.video_cover.empty()) {
            oss << "[CQ:image,file=" << snap.video_cover << ",id=40000]";
        }
    }
    else {
        oss << "未获取到视频";
        if (!snap.video_reason.empty()) {
            oss << "（" << compact_text(snap.video_reason, 84) << "）";
        }
        oss << "\n";
        if (snap.has_profile && snap.profile_archive_count >= 0) {
            oss << "公开视频 " << snap.profile_archive_count << "\n";
        }
    }

    if (snap.has_dynamic) {
        oss << "最新动态：" << compact_text(snap.dynamic_text, 120) << "\n";
        oss << "https://t.bilibili.com/" << snap.dynamic_id << "\n";
        if (!snap.dynamic_cover.empty()) {
            oss << "[CQ:image,file=" << snap.dynamic_cover << ",id=40000]";
        }
    }
    else {
        oss << "未获取到动态";
        if (!snap.dynamic_reason.empty()) {
            oss << "（" << compact_text(snap.dynamic_reason, 84) << "）";
        }
    }

    return trim(oss.str());
}

std::string biliget::list_group_subscriptions(groupid_t gid) const
{
    std::vector<userid_t> uids;
    std::unordered_map<userid_t, std::string> fallback_name_cache;
    std::unordered_map<userid_t, std::string> valid_name_cache;
    std::vector<userid_t> unresolved_uids;
    const std::time_t now = std::time(nullptr);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = group_subscriptions_.find(gid);
        if (it == group_subscriptions_.end() || it->second.empty()) {
            return fmt::format("群 {} 当前没有订阅任何 UP。", gid);
        }

        uids.assign(it->second.begin(), it->second.end());
        for (userid_t uid : uids) {
            auto c_it = up_cache_.find(uid);
            if (c_it != up_cache_.end() && !c_it->second.name.empty()) {
                fallback_name_cache[uid] = c_it->second.name;
            }

            auto n_it = list_name_cache_.find(uid);
            if (n_it != list_name_cache_.end() &&
                n_it->second.expire_at > now && !n_it->second.name.empty()) {
                valid_name_cache[uid] = n_it->second.name;
                continue;
            }

            unresolved_uids.push_back(uid);
        }
    }

    std::unordered_map<userid_t, std::string> fetched_names;
    for (userid_t uid : unresolved_uids) {
        up_snapshot_t snap;
        // Prefer UID->profile card as authoritative identity mapping.
        (void)fetch_profile_snapshot(uid, snap);
        if (snap.name.empty()) {
            (void)fetch_live_snapshot(uid, snap);
        }
        if (snap.name.empty()) {
            (void)fetch_live_snapshot_via_master(uid, snap);
        }
        if (!snap.name.empty()) {
            fetched_names[uid] = snap.name;
        }
    }

    if (!fetched_names.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::time_t expire_at = now + BILIGET_LIST_NAME_CACHE_TTL_SEC;
        for (const auto &it : fetched_names) {
            list_name_cache_[it.first] = {it.second, expire_at};
        }
    }

    if (uids.empty()) {
        return fmt::format("群 {} 当前没有订阅任何 UP。", gid);
    }

    std::ostringstream oss;
    oss << "群 " << gid << " 订阅列表(" << uids.size() << "):\n";
    for (userid_t uid : uids) {
        const std::string name = first_non_empty({
            valid_name_cache.count(uid) ? valid_name_cache[uid] : std::string(),
            fetched_names.count(uid) ? fetched_names[uid] : std::string(),
            fallback_name_cache.count(uid) ? fallback_name_cache[uid]
                                           : std::string(),
        });

        oss << "- " << uid;
        if (!name.empty()) {
            oss << " (" << name << ")";
        }
        oss << "\n";
    }
    return trim(oss.str());
}

bool biliget::send_list_group_subscriptions_forward(groupid_t gid,
                                                    const msg_meta &conf) const
{
    if (conf.p == nullptr || conf.message_type != "group") {
        return false;
    }

    const std::string text = list_group_subscriptions(gid);
    std::vector<std::string> lines;
    {
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            lines.push_back(line);
        }
        if (lines.empty()) {
            lines.push_back(text);
        }
    }

    Json::Value messages(Json::arrayValue);
    auto append_node = [&](const std::string &content) {
        Json::Value node(Json::objectValue);
        Json::Value data(Json::objectValue);
        node["type"] = "node";
        data["name"] = "Biliget";
        data["uin"] = std::to_string(conf.p->get_botqq());
        data["content"] = string_to_messageArr(content);
        node["data"] = data;
        messages.append(node);
    };

    if (lines.size() <= 1) {
        append_node(text);
    }
    else {
        const std::string header = lines.front();
        constexpr size_t kChunkLines = 30;
        for (size_t i = 1; i < lines.size(); i += kChunkLines) {
            std::ostringstream chunk;
            chunk << header;
            const size_t end = std::min(lines.size(), i + kChunkLines);
            for (size_t j = i; j < end; ++j) {
                chunk << "\n" << lines[j];
            }
            append_node(chunk.str());
        }
    }

    Json::Value req(Json::objectValue);
    req["group_id"] = Json::UInt64(gid);
    req["messages"] = messages;
    const std::string resp = conf.p->cq_send("send_group_forward_msg", req);
    const Json::Value root = string_to_json(resp);
    if (root.isObject() && root.get("status", "ok").asString() == "failed") {
        return false;
    }
    return true;
}

std::string biliget::build_live_now_message(userid_t uid,
                                            const up_snapshot_t &snap) const
{
    std::ostringstream oss;
    if (!snap.live_cover.empty()) {
        oss << "[CQ:image,file=" << snap.live_cover << ",id=40000]";
    }
    oss << "UP: " << up_name_with_uid(uid, snap.name) << "\n";
    oss << "状态: 直播中\n";
    if (!snap.live_title.empty()) {
        oss << "直播标题: " << compact_text(snap.live_title, 180) << "\n";
    }
    oss << live_jump_link(uid, snap.live_room_id);
    return trim(oss.str());
}

bool biliget::send_live_now_forward(groupid_t gid, const msg_meta &conf) const
{
    if (conf.p == nullptr || conf.message_type != "group") {
        return false;
    }

    std::vector<userid_t> uids;
    std::unordered_map<userid_t, std::string> fallback_name_cache;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = group_subscriptions_.find(gid);
        if (it == group_subscriptions_.end() || it->second.empty()) {
            return false;
        }
        uids.assign(it->second.begin(), it->second.end());
        for (userid_t uid : uids) {
            auto c_it = up_cache_.find(uid);
            if (c_it != up_cache_.end() && !c_it->second.name.empty()) {
                fallback_name_cache[uid] = c_it->second.name;
            }
        }
    }

    struct live_item_t {
        userid_t uid = 0;
        up_snapshot_t snap;
    };

    std::vector<live_item_t> live_items;
    live_items.reserve(uids.size());
    for (userid_t uid : uids) {
        up_snapshot_t snap;
        (void)fetch_live_snapshot(uid, snap);
        if (!snap.has_live) {
            (void)fetch_live_snapshot_via_master(uid, snap);
        }
        if (!snap.has_live || snap.live_status != 1) {
            continue;
        }

        userid_t final_uid = uid;
        if (snap.canonical_uid != 0) {
            final_uid = snap.canonical_uid;
        }

        if (snap.name.empty()) {
            (void)fetch_profile_snapshot(final_uid, snap);
        }
        if (snap.name.empty()) {
            auto n_it = fallback_name_cache.find(uid);
            if (n_it != fallback_name_cache.end()) {
                snap.name = n_it->second;
            }
        }

        live_items.push_back({final_uid, snap});
    }

    if (live_items.empty()) {
        return false;
    }

    Json::Value messages(Json::arrayValue);
    for (const auto &it : live_items) {
        Json::Value node(Json::objectValue);
        Json::Value data(Json::objectValue);
        node["type"] = "node";
        data["name"] = "Biliget";
        data["uin"] = std::to_string(conf.p->get_botqq());
        data["content"] =
            string_to_messageArr(build_live_now_message(it.uid, it.snap));
        node["data"] = data;
        messages.append(node);
    }

    Json::Value req(Json::objectValue);
    req["group_id"] = Json::UInt64(gid);
    req["messages"] = messages;
    const std::string resp = conf.p->cq_send("send_group_forward_msg", req);
    const Json::Value root = string_to_json(resp);
    if (root.isObject() && root.get("status", "ok").asString() == "failed") {
        return false;
    }
    return true;
}

void biliget::process(std::string message, const msg_meta &conf)
{
    const std::string raw = trim(message);
    if (raw == "bili" || raw == "biliget") {
        conf.p->cq_send(build_help_for_context(conf), conf);
        return;
    }

    std::string body;
    if (!cmd_strip_prefix(raw, "bili.", body) &&
        !cmd_strip_prefix(raw, "biliget.", body)) {
        if (has_video_token_for_decode(raw)) {
            const std::string out = biliget_decode::decode_video_text(raw);
            if (!out.empty()) {
                conf.p->cq_send(out, conf);
            }
        }
        return;
    }

    body = trim(body);

    const auto send_usage = [&]() {
        conf.p->cq_send(build_help_for_context(conf), conf);
    };

    const std::vector<cmd_exact_rule> exact_rules = {
        {"help",
         [&]() {
             conf.p->cq_send(build_help_for_context(conf), conf);
             return true;
         }},
        {"status",
         [&]() {
             std::lock_guard<std::mutex> lock(mutex_);
             size_t total_sub = 0;
             for (const auto &it : group_subscriptions_) {
                 total_sub += it.second.size();
             }
             const std::time_t now = std::time(nullptr);
             const long cb_ago = (last_poll_callback_ts_ == 0)
                                     ? -1
                                     : (now - last_poll_callback_ts_);
             const long run_ago =
                 (last_poll_run_ts_ == 0) ? -1 : (now - last_poll_run_ts_);
             conf.p->cq_send(
                 fmt::format(
                     "Bili 状态: 群数={} 订阅总数={} 轮询间隔={}s cookie={} "
                     "回调计数={} 执行计数={} 最近回调={}s 最近执行={}s",
                     group_subscriptions_.size(), total_sub, poll_interval_sec_,
                     cookie_override_.empty() ? "off" : "on",
                     poll_callback_count_, poll_run_count_, cb_ago, run_ago),
                 conf);
             return true;
         }},
        {"reload",
         [&]() {
             if (!conf.p->is_op(conf.user_id)) {
                 conf.p->cq_send("仅机器人管理员可用", conf);
                 return true;
             }
             (void)reload(conf);
             conf.p->cq_send("bili reload 完成", conf);
             return true;
         }},
        {"pollnow",
         [&]() {
             if (!conf.p->is_op(conf.user_id)) {
                 conf.p->cq_send("仅机器人管理员可用", conf);
                 return true;
             }
             {
                 std::lock_guard<std::mutex> lock(mutex_);
                 next_poll_ts_ = 0;
             }
             handle_poll(conf.p);
             conf.p->cq_send("bili 已触发一次立即轮询（若发送失败请查看日志）",
                             conf);
             return true;
         }},
    };

    const std::vector<cmd_prefix_rule> prefix_rules = {
        {"list",
         [&]() {
             if (conf.message_type != "group") {
                 conf.p->cq_send("bili.list 仅群内可用", conf);
                 return true;
             }

             groupid_t gid = conf.group_id;
             if (!send_list_group_subscriptions_forward(gid, conf)) {
                 conf.p->cq_send(list_group_subscriptions(gid), conf);
             }
             return true;
         }},
        {"live",
         [&]() {
             if (conf.message_type != "group") {
                 conf.p->cq_send("bili.live 仅群内可用", conf);
                 return true;
             }

             groupid_t gid = conf.group_id;
             if (!send_live_now_forward(gid, conf)) {
                 conf.p->cq_send(
                     "当前没有正在直播的订阅 UP，或合并转发发送失败", conf);
             }
             return true;
         }},
        {"import",
         [&]() {
             if (conf.message_type != "group") {
                 conf.p->cq_send("bili.import 仅群内可用", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "import", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             if (args.empty()) {
                 conf.p->cq_send("参数错误: bili.import <bili.list文本>", conf);
                 return true;
             }

             std::set<userid_t> uniq;
             std::istringstream iss(args);
             std::string line;
             const std::regex line_re(R"(^\s*-\s*([0-9]{1,20})\b)");
             while (std::getline(iss, line)) {
                 std::smatch m;
                 if (!std::regex_search(line, m, line_re) || m.size() < 2) {
                     continue;
                 }
                 try {
                     uniq.insert(static_cast<userid_t>(std::stoull(m.str(1))));
                 }
                 catch (...) {
                 }
             }

             if (uniq.empty()) {
                 conf.p->cq_send(
                     "未识别到可导入的 UID（请粘贴 bili.list 文本）", conf);
                 return true;
             }

             groupid_t gid = conf.group_id;
             std::lock_guard<std::mutex> lock(mutex_);
             auto &st = group_subscriptions_[gid];
             const size_t old_sz = st.size();
             st.insert(uniq.begin(), uniq.end());
             save_config_unlocked();

             conf.p->cq_send(
                 fmt::format("群 {} 已导入 {} 个 UID（新增 {}，总计 {}）", gid,
                             uniq.size(), st.size() - old_sz, st.size()),
                 conf);
             return true;
         }},
        {"add",
         [&]() {
             if (!can_modify_subscriptions(conf)) {
                 conf.p->cq_send(
                     "当前群未开放成员管理，需群管理或机器人管理员权限", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "add", args)) {
                 send_usage();
                 return true;
             }

             groupid_t gid = 0;
             std::vector<userid_t> uids;
             std::vector<std::string> failed;
             if (!parse_group_and_uids(conf, args, gid, uids, false, &failed)) {
                 conf.p->cq_send(
                     "参数错误: bili.add <uid/用户名...>（仅群内可用）", conf);
                 return true;
             }
             if (!failed.empty()) {
                 conf.p->cq_send("以下参数无法识别为 UID/用户名: " +
                                     join_tokens(failed),
                                 conf);
                 return true;
             }

             std::lock_guard<std::mutex> lock(mutex_);
             auto &st = group_subscriptions_[gid];
             size_t old_sz = st.size();
             st.insert(uids.begin(), uids.end());
             save_config_unlocked();
             conf.p->cq_send(fmt::format("群 {} 新增 {} 个订阅 (总计 {})", gid,
                                         st.size() - old_sz, st.size()),
                             conf);
             return true;
         }},
        {"del",
         [&]() {
             if (!can_modify_subscriptions(conf)) {
                 conf.p->cq_send(
                     "当前群未开放成员管理，需群管理或机器人管理员权限", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "del", args)) {
                 send_usage();
                 return true;
             }

             groupid_t gid = 0;
             std::vector<userid_t> uids;
             std::vector<std::string> failed;
             if (!parse_group_and_uids(conf, args, gid, uids, false, &failed)) {
                 conf.p->cq_send(
                     "参数错误: bili.del <uid/用户名...>（仅群内可用）", conf);
                 return true;
             }
             if (!failed.empty()) {
                 conf.p->cq_send("以下参数无法识别为 UID/用户名: " +
                                     join_tokens(failed),
                                 conf);
                 return true;
             }

             std::lock_guard<std::mutex> lock(mutex_);
             auto it = group_subscriptions_.find(gid);
             if (it == group_subscriptions_.end()) {
                 conf.p->cq_send("该群暂无订阅", conf);
                 return true;
             }

             size_t removed = 0;
             for (userid_t uid : uids) {
                 removed += it->second.erase(uid);
             }
             if (it->second.empty()) {
                 group_subscriptions_.erase(it);
             }
             save_config_unlocked();
             conf.p->cq_send(fmt::format("群 {} 删除 {} 个订阅", gid, removed),
                             conf);
             return true;
         }},
        {"set",
         [&]() {
             if (!can_modify_subscriptions(conf)) {
                 conf.p->cq_send(
                     "当前群未开放成员管理，需群管理或机器人管理员权限", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "set", args)) {
                 send_usage();
                 return true;
             }

             groupid_t gid = 0;
             std::vector<userid_t> uids;
             std::vector<std::string> failed;
             if (!parse_group_and_uids(conf, args, gid, uids, true, &failed)) {
                 conf.p->cq_send(
                     "参数错误: bili.set <uid/用户名...>（仅群内可用）", conf);
                 return true;
             }
             if (!failed.empty()) {
                 conf.p->cq_send("以下参数无法识别为 UID/用户名: " +
                                     join_tokens(failed),
                                 conf);
                 return true;
             }

             std::lock_guard<std::mutex> lock(mutex_);
             if (uids.empty()) {
                 group_subscriptions_.erase(gid);
             }
             else {
                 group_subscriptions_[gid] =
                     std::set<userid_t>(uids.begin(), uids.end());
             }
             save_config_unlocked();
             conf.p->cq_send(
                 fmt::format("群 {} 订阅已重置为 {} 个", gid, uids.size()),
                 conf);
             return true;
         }},
        {"clear",
         [&]() {
             if (!can_modify_subscriptions(conf)) {
                 conf.p->cq_send(
                     "当前群未开放成员管理，需群管理或机器人管理员权限", conf);
                 return true;
             }

             if (conf.message_type != "group") {
                 conf.p->cq_send("bili.clear 仅群内可用", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "clear", args)) {
                 send_usage();
                 return true;
             }
             if (!trim(args).empty()) {
                 conf.p->cq_send("参数错误: bili.clear", conf);
                 return true;
             }

             groupid_t gid = conf.group_id;

             std::lock_guard<std::mutex> lock(mutex_);
             group_subscriptions_.erase(gid);
             save_config_unlocked();
             conf.p->cq_send(fmt::format("群 {} 订阅已清空", gid), conf);
             return true;
         }},
        {"query",
         [&]() {
             std::string args;
             if (!cmd_strip_prefix(body, "query", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             userid_t uid = 0;
             std::string err;
             if (!resolve_target_to_uid(args, uid, err, true)) {
                 conf.p->cq_send("参数错误: bili.query <uid/房间号/用户名>",
                                 conf);
                 return true;
             }
             try {
                 conf.p->cq_send(query_one(uid), conf);
             }
             catch (const std::exception &e) {
                 conf.p->setlog(
                     LOG::ERROR,
                     fmt::format("bili.query uid={} ex={} ", uid, e.what()));
                 conf.p->cq_send("查询失败：内部异常（已记录日志）", conf);
             }
             catch (...) {
                 conf.p->setlog(
                     LOG::ERROR,
                     fmt::format("bili.query uid={} ex=unknown", uid));
                 conf.p->cq_send("查询失败：内部异常（已记录日志）", conf);
             }
             return true;
         }},
        {"debug",
         [&]() {
             std::string args;
             if (!cmd_strip_prefix(body, "debug", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             userid_t id = 0;
             std::string err;
             if (!resolve_target_to_uid(args, id, err, true)) {
                 conf.p->cq_send("参数错误: bili.debug <uid/房间号/用户名>",
                                 conf);
                 return true;
             }

             try {
                 conf.p->cq_send(biliget_debug_report(std::to_string(id)),
                                 conf);
             }
             catch (const std::exception &e) {
                 conf.p->setlog(
                     LOG::ERROR,
                     fmt::format("bili.debug id={} ex={} ", id, e.what()));
                 conf.p->cq_send("debug 失败：内部异常（已记录日志）", conf);
             }
             catch (...) {
                 conf.p->setlog(LOG::ERROR,
                                fmt::format("bili.debug id={} ex=unknown", id));
                 conf.p->cq_send("debug 失败：内部异常（已记录日志）", conf);
             }
             return true;
         }},
        {"decode",
         [&]() {
             std::string args;
             if (!cmd_strip_prefix(body, "decode", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             if (args.empty()) {
                 conf.p->cq_send("参数错误: bili.decode <BV/av/文本>", conf);
                 return true;
             }

             std::string out = biliget_decode::decode_video_text(args);
             if (out.empty()) {
                 conf.p->cq_send("未识别到有效 BV/av，或视频信息拉取失败",
                                 conf);
                 return true;
             }
             conf.p->cq_send(out, conf);
             return true;
         }},
        {"wouldpush",
         [&]() {
             std::string args;
             if (!cmd_strip_prefix(body, "wouldpush", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             userid_t uid = 0;
             std::string err;
             if (!resolve_target_to_uid(args, uid, err)) {
                 conf.p->cq_send("参数错误: bili.wouldpush <uid/用户名>", conf);
                 return true;
             }

             up_snapshot_t snap;
             try {
                 snap = fetch_snapshot(uid);
             }
             catch (...) {
                 conf.p->cq_send("wouldpush 拉取快照失败", conf);
                 return true;
             }

             up_cache_t cache;
             bool has_cache = false;
             {
                 std::lock_guard<std::mutex> lock(mutex_);
                 auto it = up_cache_.find(uid);
                 if (it != up_cache_.end()) {
                     cache = it->second;
                     has_cache = true;
                 }
             }

             bool push_dynamic = false;
             bool push_video = false;
             bool push_live_on = false;
             bool push_live_off = false;

             if (has_cache && cache.initialized) {
                 if (snap.has_dynamic && !snap.dynamic_id.empty() &&
                     snap.dynamic_id != cache.last_dynamic_id) {
                     push_dynamic = !cache.last_dynamic_id.empty();
                 }
                 if (snap.has_video && !snap.video_bvid.empty() &&
                     snap.video_bvid != cache.last_video_bvid) {
                     push_video = !cache.last_video_bvid.empty();
                 }
                 if (snap.has_live &&
                     snap.live_status != cache.last_live_status) {
                     push_live_on =
                         (cache.last_live_status != 1 && snap.live_status == 1);
                     push_live_off = (cache.last_live_status == 1);
                 }
             }

             std::ostringstream oss;
             oss << "[wouldpush]\n";
             oss << "目标: " << uid;
             if (!snap.name.empty()) {
                 oss << " (" << snap.name << ")";
             }
             oss << "\n";
             oss << "缓存: "
                 << ((has_cache && cache.initialized) ? "已初始化" : "未初始化")
                 << "\n";
             oss << "feed: cache="
                 << (cache.last_dynamic_id.empty() ? "<empty>"
                                                   : cache.last_dynamic_id)
                 << " current="
                 << (snap.dynamic_id.empty() ? "<empty>" : snap.dynamic_id)
                 << " push=" << (push_dynamic ? "yes" : "no") << "\n";
             oss << "video: cache="
                 << (cache.last_video_bvid.empty() ? "<empty>"
                                                   : cache.last_video_bvid)
                 << " current="
                 << (snap.video_bvid.empty() ? "<empty>" : snap.video_bvid)
                 << " push=" << (push_video ? "yes" : "no") << "\n";
             oss << "live: cache_status=" << cache.last_live_status
                 << " current_status=" << snap.live_status
                 << " on=" << (push_live_on ? "yes" : "no")
                 << " off=" << (push_live_off ? "yes" : "no") << "\n";
             oss << "结论: "
                 << ((push_dynamic || push_video || push_live_on ||
                      push_live_off)
                         ? "本次轮询会触发推送"
                         : "本次轮询不会触发推送");

             if (!(has_cache && cache.initialized)) {
                 oss << "（首次仅建缓存，不推送）";
             }

             conf.p->cq_send(oss.str(), conf);
             return true;
         }},
        {"perm",
         [&]() {
             if (conf.message_type != "group") {
                 conf.p->cq_send("bili.perm 仅群内可用", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "perm", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);

             if (args.empty() || args == "status") {
                 const bool open = is_group_member_manage_open(conf.group_id);
                 conf.p->cq_send(open ? "当前群成员管理: 已开放"
                                      : "当前群成员管理: 仅群管/OP",
                                 conf);
                 return true;
             }

             if (!can_manage_group(conf)) {
                 conf.p->cq_send(
                     "仅群管理或机器人管理员可调整当前群成员管理开关", conf);
                 return true;
             }

             bool new_state = false;
             if (args == "on" || args == "open" || args == "enable") {
                 new_state = true;
             }
             else if (args == "off" || args == "close" || args == "disable") {
                 new_state = false;
             }
             else {
                 conf.p->cq_send("参数错误: bili.perm status|on|off", conf);
                 return true;
             }

             {
                 std::lock_guard<std::mutex> lock(mutex_);
                 if (new_state) {
                     group_member_manage_open_.insert(conf.group_id);
                 }
                 else {
                     group_member_manage_open_.erase(conf.group_id);
                 }
                 save_config_unlocked();
             }

             conf.p->cq_send(new_state
                                 ? "已开放当前群成员管理（群友可增删改）"
                                 : "已关闭当前群成员管理（仅群管/OP可增删改）",
                             conf);
             return true;
         }},
        {"simulate",
         [&]() {
             if (!conf.p->is_op(conf.user_id)) {
                 conf.p->cq_send("仅机器人管理员可用", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "simulate", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);
             std::istringstream iss(args);
             std::string uid_s;
             std::string action;
             iss >> uid_s >> action;
             userid_t uid = 0;
             std::string err;
             if (action.empty() || !resolve_target_to_uid(uid_s, uid, err)) {
                 conf.p->cq_send("参数错误: bili.simulate <uid/用户名> "
                                 "<video|feed|live_on|live_off>",
                                 conf);
                 return true;
             }
             up_snapshot_t snap;
             try {
                 snap = fetch_snapshot(uid);
             }
             catch (...) {
                 conf.p->cq_send("simulate 拉取快照失败", conf);
                 return true;
             }

             std::string out;
             if (action == "video") {
                 if (!snap.has_video) {
                     conf.p->cq_send("simulate: 当前无可用视频快照", conf);
                     return true;
                 }
                 out = build_video_push_message(uid, snap);
             }
             else if (action == "dynamic" || action == "feed") {
                 if (!snap.has_dynamic) {
                     conf.p->cq_send("simulate: 当前无可用feed快照", conf);
                     return true;
                 }
                 out = build_dynamic_push_message(uid, snap);
             }
             else if (action == "live_on") {
                 out = build_live_on_push_message(uid, snap);
             }
             else if (action == "live_off") {
                 out = build_live_off_push_message(uid, snap);
             }
             else {
                 conf.p->cq_send("参数错误: action 仅支持 "
                                 "video|feed|live_on|live_off（兼容 dynamic）",
                                 conf);
                 return true;
             }

             conf.p->cq_send(out, conf);
             return true;
         }},
        {"interval",
         [&]() {
             if (!conf.p->is_op(conf.user_id)) {
                 conf.p->cq_send("仅机器人管理员可调整轮询间隔", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "interval", args)) {
                 send_usage();
                 return true;
             }

             args = trim(args);
             if (!str_all_digits(args)) {
                 conf.p->cq_send("参数错误: bili.interval <seconds>", conf);
                 return true;
             }

             int sec = std::stoi(args);
             sec = std::max(60, sec);

             {
                 std::lock_guard<std::mutex> lock(mutex_);
                 poll_interval_sec_ = sec;
                 next_poll_ts_ = std::time(nullptr) + 3;
                 save_config_unlocked();
             }

             conf.p->cq_send(fmt::format("轮询间隔已设置为 {} 秒", sec), conf);
             return true;
         }},
        {"cookie",
         [&]() {
             if (!conf.p->is_op(conf.user_id)) {
                 conf.p->cq_send("仅机器人管理员可用", conf);
                 return true;
             }

             std::string args;
             if (!cmd_strip_prefix(body, "cookie", args)) {
                 send_usage();
                 return true;
             }
             args = trim(args);

             if (args.empty() || args == "status") {
                 std::lock_guard<std::mutex> lock(mutex_);
                 conf.p->cq_send(
                     fmt::format("cookie={} value={}",
                                 cookie_override_.empty() ? "off" : "on",
                                 mask_cookie_text(cookie_override_)),
                     conf);
                 return true;
             }

             if (args == "clear") {
                 if (conf.message_type == "group") {
                     conf.p->cq_send(
                         "为安全起见，仅允许私聊执行 bili.cookie clear", conf);
                     return true;
                 }
                 std::lock_guard<std::mutex> lock(mutex_);
                 cookie_override_.clear();
                 biliget_http::clear_cookie_override();
                 save_cookie_secret_unlocked();
                 conf.p->cq_send("cookie 已清空", conf);
                 return true;
             }

             std::string test_target;
             if (cmd_strip_prefix(args, "test", test_target)) {
                 test_target = trim(test_target);
                 if (test_target.empty()) {
                     conf.p->cq_send("参数错误: bili.cookie test <uid/用户名>",
                                     conf);
                     return true;
                 }

                 userid_t uid = 0;
                 std::string err;
                 if (!resolve_target_to_uid(test_target, uid, err, true)) {
                     conf.p->cq_send("参数错误: bili.cookie test <uid/用户名>",
                                     conf);
                     return true;
                 }

                 const auto fmt_code_line = [](const std::string &name,
                                               const Json::Value &root,
                                               const std::string &ok_extra =
                                                   "") {
                     std::ostringstream oss;
                     oss << name << ": ";
                     if (!root.isObject()) {
                         oss << "no_json";
                         return oss.str();
                     }
                     const int code = root.get("code", -9999).asInt();
                     const std::string msg =
                         root.get("message", root.get("msg", "")).asString();
                     oss << "code=" << code;
                     if (!msg.empty()) {
                         oss << " msg=" << msg;
                     }
                     if (code == 0 && !ok_extra.empty()) {
                         oss << " " << ok_extra;
                     }
                     return oss.str();
                 };

                 Json::Value nav = safe_get_json("https://api.bilibili.com",
                                                 "/x/web-interface/nav");
                 Json::Value card = safe_get_json("https://api.bilibili.com",
                                                  "/x/web-interface/card?mid=" +
                                                      std::to_string(uid));
                 Json::Value arc = safe_get_json(
                     "https://api.bilibili.com",
                     "/x/space/arc/search?mid=" + std::to_string(uid) +
                         "&pn=1&ps=5&order=pubdate");
                 Json::Value dspace = safe_get_json(
                     "https://api.bilibili.com",
                     "/x/polymer/web-dynamic/v1/feed/space?host_mid=" +
                         std::to_string(uid));
                 Json::Value dall = safe_get_json(
                     "https://api.bilibili.com",
                     "/x/polymer/web-dynamic/v1/feed/all?type=all&host_mid=" +
                         std::to_string(uid));
                 Json::Value dsvr =
                     safe_get_json("https://api.vc.bilibili.com",
                                   "/dynamic_svr/v1/dynamic_svr/"
                                   "space_history?visitor_uid=0&host_uid=" +
                                       std::to_string(uid) +
                                       "&offset_dynamic_id=0&need_top=1");

                 std::string dall_extra;
                 if (dall.isObject() && dall.get("code", -1).asInt() == 0) {
                     const Json::Value items = dall["data"]["items"];
                     size_t cnt = (items.isArray() ? items.size() : 0);
                     std::string bvid;
                     if (items.isArray()) {
                         for (const auto &it : items) {
                             const Json::Value archive =
                                 it["modules"]["module_dynamic"]["major"]
                                   ["archive"];
                             if (archive.isObject()) {
                                 bvid = archive.get("bvid", "").asString();
                                 if (!bvid.empty()) {
                                     break;
                                 }
                             }
                         }
                     }
                     dall_extra = fmt::format("items={} bvid={}", cnt,
                                              bvid.empty() ? "<none>" : bvid);
                 }

                 std::string dsvr_extra;
                 if (dsvr.isObject() && dsvr.get("code", -1).asInt() == 0) {
                     const Json::Value cards = dsvr["data"]["cards"];
                     dsvr_extra = fmt::format(
                         "cards={}", cards.isArray() ? cards.size() : 0);
                 }

                 std::ostringstream oss;
                 oss << "[cookie test] target=" << uid << "\n";
                 if (card.isObject() && card.get("code", -1).asInt() == 0) {
                     oss << "uname="
                         << card["data"]["card"].get("name", "").asString()
                         << "\n";
                 }
                 oss << fmt_code_line(
                            "nav", nav,
                            fmt::format(
                                "isLogin={} cookie={}",
                                nav["data"].get("isLogin", false).asBool()
                                    ? "true"
                                    : "false",
                                cookie_override_.empty() ? "off" : "on"))
                     << "\n";
                 oss << fmt_code_line("space.arc.search", arc) << "\n";
                 oss << fmt_code_line("dynamic.feed.space", dspace) << "\n";
                 oss << fmt_code_line("dynamic.feed.all", dall, dall_extra)
                     << "\n";
                 oss << fmt_code_line("dynamic_svr.space_history", dsvr,
                                      dsvr_extra)
                     << "\n";
                 oss << "判定: 若仅 arc/space 为 -412 但 feed.all 与 "
                        "dynamic_svr 为 0，"
                        "通常是端点级风控(与 cookie+IP "
                        "组合相关)，可继续走回退链。";

                 conf.p->cq_send(oss.str(), conf);
                 return true;
             }

             std::string cookie;
             if (!cmd_strip_prefix(args, "set", cookie)) {
                 conf.p->cq_send(
                     "用法: bili.cookie status | bili.cookie set <cookie> | "
                     "bili.cookie clear | bili.cookie test <uid/用户名>",
                     conf);
                 return true;
             }
             cookie = trim(cookie);
             if (cookie.empty()) {
                 conf.p->cq_send("请提供 cookie 内容", conf);
                 return true;
             }

             if (conf.message_type == "group") {
                 conf.p->cq_send("为安全起见，仅允许私聊执行 bili.cookie set",
                                 conf);
                 return true;
             }

             {
                 std::lock_guard<std::mutex> lock(mutex_);
                 cookie_override_ = cookie;
                 biliget_http::set_cookie_override(cookie_override_);
                 save_cookie_secret_unlocked();
             }

             conf.p->cq_send(
                 fmt::format("cookie 已设置: {}", mask_cookie_text(cookie)),
                 conf);
             return true;
         }},
    };

    bool handled = false;
    (void)cmd_try_dispatch(body, exact_rules, prefix_rules, handled);
    if (!handled) {
        send_usage();
    }
}

bool biliget::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    const std::string m = trim(message);
    if (m == "bili" || m == "biliget" ||
        cmd_match_prefix(m, {"bili.", "biliget."})) {
        return true;
    }
    return has_video_token_for_decode(m);
}

bool biliget::reload(const msg_meta &conf)
{
    (void)conf;
    std::lock_guard<std::mutex> lock(mutex_);
    load_config_unlocked();
    load_cache_unlocked();
    load_cookie_secret_unlocked();
    next_poll_ts_ = std::time(nullptr) + 3;
    return true;
}

std::string biliget::help() { return "B站订阅推送与查询。完整命令：bili.help"; }

std::string biliget::help(const msg_meta &conf, help_level_t level)
{
    (void)conf;
    (void)level;
    return help();
}

void biliget::set_callback(std::function<void(std::function<void(bot *p)>)> f)
{
    f([this](bot *p) { handle_poll(p); });
}

DECLARE_FACTORY_FUNCTIONS(biliget)
