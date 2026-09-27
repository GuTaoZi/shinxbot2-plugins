#include "poke.h"
#include "utils.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>

namespace {

const char *HITOKOTO_STATE_PATH = "features/poke/poke_state.json";

std::string today_date_string()
{
    std::time_t now = std::time(nullptr);
    std::tm local_tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

std::string build_hitokoto_message(const Json::Value &root)
{
    const std::string hitokoto = trim(root.get("hitokoto", "").asString());
    if (hitokoto.empty()) {
        return "戳戳你喵~";
    }

    const std::string from = trim(root.get("from", "").asString());
    const std::string from_who = trim(root.get("from_who", "").asString());

    std::string suffix;
    if (!from.empty() && !from_who.empty()) {
        suffix = "——" + from_who + "《" + from + "》";
    }
    else if (!from.empty()) {
        suffix = "——《" + from + "》";
    }
    else if (!from_who.empty()) {
        suffix = "——" + from_who;
    }

    if (suffix.empty()) {
        return hitokoto;
    }
    return hitokoto + "\n" + suffix;
}

std::string fetch_hitokoto_with_retry()
{
    const std::map<std::string, std::string> headers = {
        {"User-Agent", "shinxbot-poke/1.0"},
        {"Accept", "application/json"},
    };

    for (int attempt = 0; attempt < 6; ++attempt) {
        try {
            const std::string body =
                do_get("https://v1.hitokoto.cn",
                       "/?c=a&c=b&c=c&c=d&c=i&c=k&c=l&encode=json", true,
                       headers, false);
            const Json::Value root = string_to_json(body);
            const std::string msg = build_hitokoto_message(root);
            if (!msg.empty()) {
                return msg;
            }
        }
        catch (...) {
            // Retry with backoff when request fails or gets limited.
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(250 * (attempt + 1)));
    }
    return "今天也要元气满满喵~";
}

int send_likes_back(bot *p, userid_t user_id, int desired)
{
    int sent = 0;
    for (int i = 0; i < desired; ++i) {
        Json::Value req;
        req["user_id"] = Json::UInt64(user_id);
        req["times"] = 1;

        const Json::Value resp = string_to_json(p->cq_send("send_like", req));
        if (!resp.isObject() || resp.get("status", "").asString() != "ok") {
            break;
        }
        ++sent;
    }
    return sent;
}

} // namespace

poke::poke()
{
    Json::Value J = string_to_json(
        readfile(bot_config_path(nullptr, "features/poke/poke.json"),
                 "{\"users\": [], \"groups\": []}"));
    parse_json_to_set(J["users"], no_poke_users);
    parse_json_to_set(J["groups"], no_poke_groups);
    minInterval = std::chrono::seconds(3);
    load_hitokoto_state();
}

void poke::load_hitokoto_state()
{
    hitokoto_date_ = today_date_string();
    hitokoto_users_today_.clear();
    Json::Value J = string_to_json(
        readfile(bot_config_path(nullptr, HITOKOTO_STATE_PATH), "{}"));
    if (!J.isObject() || J.get("date", "").asString() != hitokoto_date_) {
        return;
    }
    parse_json_to_set(J["users"], hitokoto_users_today_);
}

void poke::save_hitokoto_state() const
{
    Json::Value J(Json::objectValue);
    J["date"] = hitokoto_date_;
    Json::Value users(Json::arrayValue);
    for (userid_t u : hitokoto_users_today_) {
        users.append(Json::UInt64(u));
    }
    J["users"] = users;
    writefile(bot_config_path(nullptr, HITOKOTO_STATE_PATH),
              J.toStyledString(), false);
}

// True (and records the user) if they haven't had today's hitokoto yet.
bool poke::take_daily_hitokoto(userid_t user_id)
{
    const std::string today = today_date_string();
    if (hitokoto_date_ != today) {
        hitokoto_date_ = today;
        hitokoto_users_today_.clear();
    }
    if (!hitokoto_users_today_.insert(user_id).second) {
        return false;
    }
    save_hitokoto_state();
    return true;
}

void poke::process(bot *p, Json::Value J)
{
    std::unique_lock<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    if (lastExecutionTime_.time_since_epoch().count() != 0) {
        auto elapsedTime = now - lastExecutionTime_;
        if (elapsedTime < minInterval) {
            return;
        }
    }
    lastExecutionTime_ = std::chrono::steady_clock::now();

    // p->cq_send("[CQ:poke,qq=" + std::to_string(J["user_id"].asUInt64()) +
    // "]",
    //         (msg_meta){"group", 0, J["group_id"].asUInt64(), 0});
    const userid_t user_id = J["user_id"].asUInt64();
    const groupid_t group_id = J["group_id"].asUInt64();

    Json::Value Jx;
    Jx["group_id"] = Json::UInt64(group_id);
    Jx["user_id"] = Json::UInt64(user_id);
    p->cq_send("group_poke", Jx);

    // Likes are a bonus: they fail for users whose privacy settings block
    // them, and after QQ's per-day cap — neither should cost the hitokoto.
    send_likes_back(p, user_id, 20);
    if (!take_daily_hitokoto(user_id)) {
        return;
    }
    const std::string hitokoto = fetch_hitokoto_with_retry();
    const std::string reply = hitokoto;
    p->cq_send(reply, (msg_meta){"group", 0, group_id, 0});
}
bool poke::check(bot *p, Json::Value J)
{
    if (!J.isMember("post_type") || !J.isMember("notice_type") ||
        !J.isMember("sub_type") || !J.isMember("group_id") ||
        !J.isMember("user_id") || !J.isMember("target_id")) {
        return false;
    }
    if (J["post_type"].asString() != "notice" ||
        J["notice_type"].asString() != "notify" ||
        J["sub_type"].asString() != "poke") {
        return false;
    }
    userid_t target_id = J["target_id"].asUInt64();
    userid_t user_id = J["user_id"].asUInt64();
    groupid_t group_id = J["group_id"].asUInt64();
    if (target_id != p->get_botqq() ||
        no_poke_users.find(user_id) != no_poke_users.end() ||
        no_poke_groups.find(group_id) != no_poke_groups.end())
        return false;
    return true;
}

DECLARE_FACTORY_FUNCTIONS(poke)