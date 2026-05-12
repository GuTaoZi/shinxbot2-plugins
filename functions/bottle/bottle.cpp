#include "bottle.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string bottle_help_text()
{
    return "漂流瓶\n"
           "*bottle.help: 查看帮助\n"
           "*扔 <text>: 扔一个漂流瓶\n"
           "*捞: 捞一个漂流瓶\n"
           "*bottle.cd 扔/捞 <秒>";
}

bool can_set_cd(const msg_meta &conf)
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

bool all_digits(const std::string &s)
{
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool validate_bottle_text(const std::string &text, std::string &err)
{
    static const std::unordered_set<std::string> allowed = {
        "face",
        "sface",
        "emoji",
    };
    static const std::unordered_map<std::string, std::string> blocked_hint = {
        {"image", "图片"},     {"record", "语音"}, {"video", "视频"},
        {"file", "文件"},      {"xml", "XML"},     {"json", "JSON"},
        {"share", "分享卡片"},
    };

    size_t pos = 0;
    while (true) {
        pos = text.find("[CQ:", pos);
        if (pos == std::string::npos) {
            return true;
        }

        const size_t close = text.find(']', pos + 4);
        if (close == std::string::npos) {
            err = "CQ 码格式不完整。";
            return false;
        }

        const size_t type_begin = pos + 4;
        size_t type_end = text.find(',', type_begin);
        if (type_end == std::string::npos || type_end > close) {
            type_end = close;
        }

        std::string type = text.substr(type_begin, type_end - type_begin);
        type = trim(type);
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });

        if (type.empty()) {
            err = "CQ 码类型为空。";
            return false;
        }
        if (allowed.find(type) == allowed.end()) {
            auto it = blocked_hint.find(type);
            if (it != blocked_hint.end()) {
                err = "不支持 " + it->second + " CQ 码。";
            }
            else {
                err = "该 CQ 码类型暂不支持：" + type;
            }
            return false;
        }

        pos = close + 1;
    }
}

std::string json_escape_keep_utf8(const std::string &s)
{
    std::ostringstream oss;
    for (unsigned char ch : s) {
        switch (ch) {
        case '"':
            oss << "\\\"";
            break;
        case '\\':
            oss << "\\\\";
            break;
        case '\b':
            oss << "\\b";
            break;
        case '\f':
            oss << "\\f";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (ch < 0x20) {
                oss << "\\u" << std::uppercase << std::hex << std::setw(4)
                    << std::setfill('0') << (int)ch << std::nouppercase
                    << std::dec;
            }
            else {
                oss << static_cast<char>(ch);
            }
            break;
        }
    }
    return oss.str();
}

} // namespace

void react_or_reply(const msg_meta &conf, const std::string &emoji_id,
                    const std::string &fallback_text)
{
    bool ok = false;
    try {
        Json::Value j;
        j["message_id"] = conf.message_id;
        j["emoji_id"] = emoji_id;
        Json::Value r =
            string_to_json(conf.p->cq_send("set_msg_emoji_like", j));
        ok = r["status"].asString() == "ok";
    }
    catch (...) {
    }

    if (!ok && !fallback_text.empty()) {
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) +
                            "]" + fallback_text,
                        conf);
    }
}

bottle::bottle()
{
    std::lock_guard<std::mutex> lock(mutex_);
    load_unlocked();
}

bottle::cd_config &bottle::get_cd_config_unlocked(groupid_t gid)
{
    return group_cd_[gid]; // 默认自动构造
}

bool bottle::check_cd_and_notify(groupid_t gid, userid_t uid, int64_t now,
                                 int64_t last_time, int cd_sec,
                                 const msg_meta &conf)
{
    if (now - last_time < cd_sec) {
        auto &notify_map = last_cd_notify_time_[gid];
        int64_t &last_notify = notify_map[uid];

        if (now - last_notify >= 60) {
            last_notify = now;

            std::string msg = "当前冷却时间为 " + std::to_string(cd_sec) +
                              " 秒，请稍后再试。";

            react_or_reply(conf, "424", msg);
        }
        else {
            react_or_reply(conf, "424", "");
        }

        return false;
    }
    return true;
}

// ===================== 数据加载 =====================

void bottle::load_unlocked()
{
    bottles_.clear();
    mutations_since_dedup_ = 0;

    const Json::Value root = string_to_json(readfile(storage_path_, "{}"));
    const Json::Value arr = root.get("bottles", Json::Value(Json::arrayValue));
    if (!arr.isArray()) {
        return;
    }

    for (const auto &it : arr) {
        if (!it.isObject())
            continue;

        bottle_item one;
        one.text = trim(cq_decode(it.get("text", "").asString()));
        one.created_at = it.get("created_at", 0).asInt64();
        if (it["sender_user_id"].isUInt64()) {
            one.sender_user_id = it["sender_user_id"].asUInt64();
        }
        else if (it["sender_user_id"].isString()) {
            const std::string sid = trim(it["sender_user_id"].asString());
            if (all_digits(sid)) {
                one.sender_user_id = static_cast<userid_t>(std::stoull(sid));
            }
        }
        if (it["sender_group_id"].isUInt64()) {
            one.sender_group_id = it["sender_group_id"].asUInt64();
        }
        else if (it["sender_group_id"].isString()) {
            const std::string gid = trim(it["sender_group_id"].asString());
            if (all_digits(gid)) {
                one.sender_group_id = static_cast<groupid_t>(std::stoull(gid));
            }
        }
        one.sender_message_type = it.get("sender_message_type", "").asString();

        if (one.text.empty())
            continue;
        bottles_.push_back(std::move(one));
    }

    if (bottles_.size() > kMaxBottleCount) {
        bottles_.erase(bottles_.begin(),
                       bottles_.begin() + static_cast<long>(bottles_.size() -
                                                            kMaxBottleCount));
    }

    dedup_bottles_unlocked();
    last_dedup_at_ = static_cast<int64_t>(std::time(nullptr));
}

void bottle::save_unlocked() const
{
    std::ostringstream oss;
    oss << "{\n  \"bottles\" : [\n";

    for (size_t i = 0; i < bottles_.size(); ++i) {
        const auto &it = bottles_[i];
        oss << "    {\n";
        oss << "      \"created_at\" : " << it.created_at << ",\n";
        oss << "      \"sender_user_id\" : " << it.sender_user_id << ",\n";
        oss << "      \"sender_group_id\" : " << it.sender_group_id << ",\n";
        oss << "      \"sender_message_type\" : \""
            << json_escape_keep_utf8(it.sender_message_type) << "\",\n";
        oss << "      \"text\" : \"" << json_escape_keep_utf8(it.text)
            << "\"\n";
        oss << "    }";
        if (i + 1 < bottles_.size()) {
            oss << ",";
        }
        oss << "\n";
    }

    oss << "  ]\n}\n";
    writefile(storage_path_, oss.str(), false);
}

void bottle::dedup_bottles_unlocked()
{
    if (bottles_.size() <= 1) {
        return;
    }

    std::unordered_set<std::string> seen;
    seen.reserve(bottles_.size() * 2);

    std::vector<bottle_item> kept;
    kept.reserve(bottles_.size());

    // Reverse traversal keeps the newest duplicate and drops older ones.
    for (auto it = bottles_.rbegin(); it != bottles_.rend(); ++it) {
        if (!seen.insert(it->text).second) {
            continue;
        }
        kept.push_back(*it);
    }

    std::reverse(kept.begin(), kept.end());
    bottles_.swap(kept);
}

void bottle::maybe_dedup_unlocked(int64_t now_sec, bool force)
{
    if (!force && now_sec - last_dedup_at_ < kDedupIntervalSec &&
        mutations_since_dedup_ < 50) {
        return;
    }
    dedup_bottles_unlocked();
    last_dedup_at_ = now_sec;
    mutations_since_dedup_ = 0;
}

// ===================== 核心逻辑 =====================

void bottle::process(std::string message, const msg_meta &conf)
{
    const std::string raw = trim(message);

    if (raw == "*bottle.help") {
        conf.p->cq_send(bottle_help_text(), conf);
        return;
    }

    // ===== CD 设置 =====
    std::string cd_args;
    if (cmd_strip_prefix(raw, "*bottle.cd", cd_args)) {
        std::istringstream iss(cd_args);
        std::string type;
        int sec;

        if (!(iss >> type >> sec) || sec < 0) {
            conf.p->cq_send("用法: *bottle.cd 扔/捞 <秒>", conf);
            return;
        }

        if (!can_set_cd(conf)) {
            conf.p->cq_send("权限不足。", conf);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &cfg = get_cd_config_unlocked(conf.group_id);

            if (type == "扔") {
                cfg.drop_cd_sec = sec;
            }
            else if (type == "捞") {
                cfg.pick_cd_sec = sec;
            }
            else {
                conf.p->cq_send("类型只能是 扔 或 捞。", conf);
                return;
            }
        }

        conf.p->cq_send("已更新冷却时间。", conf);
        return;
    }

    // ===== 扔 =====
    std::string drop_args;
    if (cmd_strip_prefix(raw, "*扔", drop_args)) {
        std::string text = trim(drop_args);

        if (text.empty()) {
            conf.p->cq_send("用法: *扔 <text>", conf);
            return;
        }

        if (text.size() > kMaxBottleTextLen) {
            conf.p->cq_send("漂流瓶塞不下这么长的纸条~", conf);
            return;
        }

        std::string err;
        if (!validate_bottle_text(text, err)) {
            conf.p->cq_send(err, conf);
            return;
        }

        const int64_t now = std::time(nullptr);

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto &cfg = get_cd_config_unlocked(conf.group_id);
            auto &user_map = last_drop_time_[conf.group_id];

            if (!check_cd_and_notify(conf.group_id, conf.user_id, now,
                                     user_map[conf.user_id], cfg.drop_cd_sec,
                                     conf)) {
                return;
            }

            user_map[conf.user_id] = now;

            if (bottles_.size() >= kMaxBottleCount) {
                bottles_.erase(bottles_.begin());
            }

            bottles_.push_back(
                {text, now, conf.user_id, conf.group_id, conf.message_type});
            ++mutations_since_dedup_;
            maybe_dedup_unlocked(now, false);
            save_unlocked();
        }

        conf.p->cq_send("已匿名投递漂流瓶。", conf);
        return;
    }

    // ===== 捞 =====
    if (raw == "*捞") {
        const int64_t now = std::time(nullptr);

        std::string text;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto &cfg = get_cd_config_unlocked(conf.group_id);
            auto &user_map = last_pick_time_[conf.group_id];

            if (!check_cd_and_notify(conf.group_id, conf.user_id, now,
                                     user_map[conf.user_id], cfg.pick_cd_sec,
                                     conf)) {
                return;
            }

            user_map[conf.user_id] = now;

            if (bottles_.empty()) {
                conf.p->cq_send("海里还没有漂流瓶。", conf);
                return;
            }

            const int idx = get_random(static_cast<int>(bottles_.size()));
            text = bottles_[idx].text;
            bottles_.erase(bottles_.begin() + idx);
            ++mutations_since_dedup_;
            maybe_dedup_unlocked(now, false);

            save_unlocked();
        }

        conf.p->cq_send("你捞到一个匿名漂流瓶：\n" + text, conf);
        return;
    }

    conf.p->cq_send(bottle_help_text(), conf);
}

// ===================== 其他 =====================

bool bottle::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    return cmd_match_prefix(trim(message),
                            {"*扔", "*捞", "*bottle.help", "*bottle.cd"});
}

bool bottle::reload(const msg_meta &conf)
{
    (void)conf;
    std::lock_guard<std::mutex> lock(mutex_);

    last_drop_time_.clear();
    last_pick_time_.clear();
    last_cd_notify_time_.clear();

    load_unlocked();
    maybe_dedup_unlocked(static_cast<int64_t>(std::time(nullptr)), true);
    save_unlocked();
    return true;
}

std::string bottle::help() { return "漂流瓶：*bottle.help"; }

DECLARE_FACTORY_FUNCTIONS(bottle)