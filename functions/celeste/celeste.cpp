#include "celeste.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <fmt/format.h>
#include <mutex>
#include <sstream>

namespace {
const std::string kConfig = "features/celeste/celeste.json";
const std::string kAliases = "features/celeste/aliases.json";
const std::string kGroups = "features/celeste/groups.json";

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    static_cast<std::string *>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string first_token(const std::string &s, std::string &rest)
{
    size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) {
        rest.clear();
        return s;
    }
    rest = trim(s.substr(sp + 1));
    return s.substr(0, sp);
}

std::string url_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        }
        else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

// Escape CQ-code specials so literal text (e.g. names with '[') survives
// string_to_messageArr, which otherwise parses '[' as the start of a CQ code.
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

// If the query ends with a standalone count token — "10", "10条", "+10", "-10"
// — strip it and return the count (clamped 1..50); else return 0. Never eats a
// single-token query, so "gold map 7d1d" keeps "7d1d" as the name.
int trailing_count(std::string &q)
{
    const std::string s = trim(q);
    const size_t sp = s.find_last_of(" \t");
    if (sp == std::string::npos) {
        return 0;
    }
    std::string tok = s.substr(sp + 1);
    for (const char *suf : {"条", "个", "条记录"}) {
        const size_t n = std::string(suf).size();
        if (tok.size() > n && tok.compare(tok.size() - n, n, suf) == 0) {
            tok = tok.substr(0, tok.size() - n);
            break;
        }
    }
    size_t i = 0;
    if (i < tok.size() && (tok[i] == '+' || tok[i] == '-')) {
        ++i;
    }
    if (i >= tok.size()) {
        return 0;
    }
    for (size_t k = i; k < tok.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(tok[k]))) {
            return 0;
        }
    }
    int n = 0;
    try {
        n = std::stoi(tok.substr(i));
    }
    catch (...) {
        return 0;
    }
    if (n <= 0) {
        return 0;
    }
    q = trim(s.substr(0, sp));
    return std::min(n, 50);
}

// A player's clear rendered as one line, keyed by date for sorting.
std::string clear_date(const Json::Value &s)
{
    if (s.isMember("date_achieved") && !s["date_achieved"].isNull()) {
        return s["date_achieved"].asString();
    }
    return s.get("date_created", "").asString();
}
} // namespace

celeste::celeste()
{
    load_config_unlocked();
    load_aliases_unlocked();
    load_groups_unlocked();
}

void celeste::load_config_unlocked()
{
    Json::Value j = string_to_json(readfile(bot_config_path(nullptr, kConfig), "{}"));
    if (j.isObject()) {
        base_url_ = j.get("base_url", base_url_).asString();
        cache_ttl_sec_ = j.get("cache_ttl_sec", cache_ttl_sec_).asInt();
        default_recent_ = std::max(1, j.get("default_recent", default_recent_).asInt());
    }
}

void celeste::load_aliases_unlocked()
{
    aliases_.clear();
    Json::Value j = string_to_json(readfile(bot_config_path(nullptr, kAliases), "{}"));
    if (j.isObject()) {
        for (const auto &k : j.getMemberNames()) {
            aliases_[normalize(k)] = j[k].asString();
        }
    }
}

void celeste::save_aliases_unlocked() const
{
    Json::Value j(Json::objectValue);
    for (const auto &kv : aliases_) {
        j[kv.first] = kv.second;
    }
    writefile(bot_config_path(nullptr, kAliases), j.toStyledString(), false);
}

void celeste::load_groups_unlocked()
{
    group_recent_.clear();
    Json::Value j = string_to_json(readfile(bot_config_path(nullptr, kGroups), "{}"));
    if (j.isObject()) {
        for (const auto &k : j.getMemberNames()) {
            try {
                group_recent_[std::stoll(k)] = j[k].asInt();
            }
            catch (...) {
            }
        }
    }
}

void celeste::save_groups_unlocked() const
{
    Json::Value j(Json::objectValue);
    for (const auto &kv : group_recent_) {
        j[std::to_string(kv.first)] = kv.second;
    }
    writefile(bot_config_path(nullptr, kGroups), j.toStyledString(), false);
}

int celeste::recent_for(const msg_meta &conf) const
{
    auto it = group_recent_.find(static_cast<long long>(conf.group_id));
    return it != group_recent_.end() ? it->second : default_recent_;
}

std::string celeste::http_get(const std::string &url)
{
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    thread_local CURL *h = curl_easy_init();
    if (h == nullptr) {
        return "";
    }
    curl_easy_reset(h);
    std::string out;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_USERAGENT, "shinxbot-celeste/1.0");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    if (curl_easy_perform(h) != CURLE_OK) {
        return "";
    }
    return out;
}

std::string celeste::normalize(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 0x80) {
            out.push_back(static_cast<char>(c)); // keep CJK / unicode bytes
        }
        else if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        // ASCII spaces/punctuation dropped
    }
    return out;
}

bool celeste::ensure_cache()
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::time_t now = std::time(nullptr);
    if (!maps_.empty() && now - cache_ts_ < cache_ttl_sec_) {
        return true;
    }
    return fetch_and_build_unlocked();
}

bool celeste::fetch_and_build_unlocked()
{
    const std::string body = http_get(base_url_ + "/api/lists/top-golden-list.php");
    if (body.empty()) {
        return false;
    }
    Json::Value root = string_to_json(body);
    if (!root.isObject() || !root.isMember("maps")) {
        return false;
    }

    maps_.clear();
    campaigns_.clear();
    map_tier_.clear();
    map_name_idx_.clear();
    tier_maps_.clear();

    const Json::Value &campaigns = root["campaigns"];
    for (const auto &k : campaigns.getMemberNames()) {
        const Json::Value &c = campaigns[k];
        campaigns_[c.get("id", 0).asInt()] = c.get("name", "").asString();
    }

    const Json::Value &maps = root["maps"];
    for (const auto &k : maps.getMemberNames()) {
        const Json::Value &m = maps[k];
        MapInfo mi;
        mi.id = m.get("id", 0).asInt();
        mi.name = m.get("name", "").asString();
        mi.campaign_id = m.get("campaign_id", 0).asInt();
        if (mi.id != 0) {
            maps_[mi.id] = mi;
            map_name_idx_.emplace_back(normalize(mi.name), mi.id);
            // also let users search by the campaign (地图包) name
            auto cit = campaigns_.find(mi.campaign_id);
            if (cit != campaigns_.end() && !cit->second.empty()) {
                const std::string cn = normalize(cit->second);
                if (cn != normalize(mi.name)) {
                    map_name_idx_.emplace_back(cn, mi.id);
                }
            }
        }
    }

    const Json::Value &challenges = root["challenges"];
    for (const auto &ch : challenges) {
        if (!ch.isMember("map_id") || ch["map_id"].isNull()) {
            continue; // v1 handles map-level challenges only
        }
        const int map_id = ch["map_id"].asInt();
        const std::string tier = ch["difficulty"].get("name", "").asString();
        const bool prefer = (ch["objective"].get("name", "").asString() == "Golden Berry");
        // map_tier_ is only a quick hint (e.g. for tier listing); prefer the
        // Golden Berry challenge's tier when a map has several challenges.
        if (map_tier_.find(map_id) == map_tier_.end() || prefer) {
            map_tier_[map_id] = tier;
        }
        if (!tier.empty()) {
            tier_maps_[tier].push_back(map_id);
        }
    }

    for (auto &kv : tier_maps_) {
        auto &v = kv.second;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    cache_ts_ = std::time(nullptr);
    return true;
}

std::vector<int> celeste::resolve_maps(const std::string &q) const
{
    std::string nq = normalize(q);
    auto ait = aliases_.find(nq); // community nickname -> canonical map name
    if (ait != aliases_.end()) {
        nq = normalize(ait->second);
    }
    if (nq.empty()) {
        return {};
    }
    std::vector<int> exact, prefix, sub;
    for (const auto &pr : map_name_idx_) {
        const std::string &n = pr.first;
        if (n == nq) {
            exact.push_back(pr.second);
        }
        else if (n.rfind(nq, 0) == 0) {
            prefix.push_back(pr.second);
        }
        else if (n.find(nq) != std::string::npos) {
            sub.push_back(pr.second);
        }
    }
    std::vector<int> &res = !exact.empty() ? exact : (!prefix.empty() ? prefix : sub);
    if (res.size() > 12) {
        res.resize(12);
    }
    return res;
}

void celeste::send_lines(const msg_meta &conf, const std::string &title,
                         const std::vector<std::string> &lines)
{
    std::ostringstream full;
    full << title;
    for (const auto &l : lines) {
        full << "\n" << l;
    }

    // Keep short replies inline; fold long ones (multi-challenge maps, big
    // tier lists, large recent-N) into a merged-forward message.
    constexpr size_t kFoldOver = 10;
    if (lines.size() <= kFoldOver) {
        cq_send(conf.p, full.str(), conf);
        return;
    }

    // Fold into a QQ merged-forward message, chunked into nodes — same approach
    // as the 美图 plugin, but works in both group and private chats.
    Json::Value messages(Json::arrayValue);
    const std::string uin = std::to_string(conf.p->get_botqq());
    auto node = [&](const std::string &content) {
        Json::Value n(Json::objectValue), d(Json::objectValue);
        n["type"] = "node";
        d["name"] = "Celeste";
        d["uin"] = uin;
        d["content"] = string_to_messageArr(cq_escape(content));
        n["data"] = d;
        messages.append(n);
    };
    node(title);
    constexpr size_t kChunk = 20;
    for (size_t i = 0; i < lines.size(); i += kChunk) {
        std::ostringstream chunk;
        const size_t end = std::min(lines.size(), i + kChunk);
        for (size_t j = i; j < end; ++j) {
            if (j > i) {
                chunk << "\n";
            }
            chunk << lines[j];
        }
        node(chunk.str());
    }
    Json::Value req(Json::objectValue);
    std::string endpoint;
    if (conf.message_type == "group") {
        req["group_id"] = Json::UInt64(conf.group_id);
        endpoint = "send_group_forward_msg";
    }
    else {
        req["user_id"] = Json::UInt64(conf.user_id);
        endpoint = "send_private_forward_msg";
    }
    req["messages"] = messages;
    const std::string resp = cq_send(conf.p, endpoint, req);
    const Json::Value r = string_to_json(resp);
    if (!r.isObject() || r.get("status", "ok").asString() == "failed") {
        cq_send(conf.p, full.str(), conf); // fallback to plain
    }
}

void celeste::cmd_map(const std::string &q, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "goldberries 数据获取失败，稍后再试。", conf);
        return;
    }
    std::string query = q;
    const int reqn = trailing_count(query);
    std::vector<int> ids;
    std::string name_of, camp_of;
    int recent = default_recent_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        recent = reqn > 0 ? reqn : recent_for(conf);
        ids = resolve_maps(query);
        if (ids.size() == 1) {
            const int id = ids[0];
            name_of = maps_[id].name;
            camp_of = campaigns_.count(maps_[id].campaign_id)
                          ? campaigns_[maps_[id].campaign_id]
                          : "";
        }
    }

    if (ids.empty()) {
        cq_send(conf.p, fmt::format("没找到地图「{}」。试试 gold ? {}", query, query),
                conf);
        return;
    }
    if (ids.size() > 1) {
        std::vector<std::string> cand;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (int id : ids) {
                const std::string c = campaigns_.count(maps_[id].campaign_id)
                                          ? campaigns_[maps_[id].campaign_id]
                                          : "";
                cand.push_back(fmt::format("{}. {}{}", cand.size() + 1,
                                           maps_[id].name,
                                           c.empty() ? "" : " (" + c + ")"));
            }
        }
        send_lines(conf, fmt::format("找到多个地图匹配「{}」，请更精确：", query), cand);
        return;
    }

    // Fetch full detail: all challenges + all clearers (golden-list only carries
    // one representative submission per challenge).
    const int id = ids[0];
    const Json::Value d = string_to_json(http_get(
        base_url_ +
        fmt::format("/api/map/map.php?id={}&challenges=true&submissions=true", id)));
    const std::string base_title =
        fmt::format("「{}」{}", name_of, camp_of.empty() ? "" : " (" + camp_of + ")");
    const Json::Value &chs = d["challenges"];
    if (!chs.isArray() || chs.empty()) {
        cq_send(conf.p, base_title + "\n暂无难度/通关记录。", conf);
        return;
    }

    // Golden Berry challenge(s) first, then the rest.
    std::vector<const Json::Value *> order;
    for (const auto &c : chs) {
        if (c["objective"].get("name", "").asString() == "Golden Berry") {
            order.push_back(&c);
        }
    }
    for (const auto &c : chs) {
        if (c["objective"].get("name", "").asString() != "Golden Berry") {
            order.push_back(&c);
        }
    }

    // Link below the name, pointing at the primary (golden) challenge.
    const std::string title = fmt::format("{}\n{}/map/{}/{}", base_title,
                                          base_url_, id, (*order[0])["id"].asInt());
    std::vector<std::string> lines;
    for (const Json::Value *cp : order) {
        const Json::Value &c = *cp;
        const std::string obj = c["objective"].get("name", "").asString();
        const std::string tier = c["difficulty"].get("name", "未定级").asString();
        const Json::Value &subs = c["submissions"];
        const int n = subs.isArray() ? static_cast<int>(subs.size()) : 0;
        lines.push_back(fmt::format(
            "── {} · {} · 通关 {} 条{}", obj, tier, n,
            n > 0 ? fmt::format("，最近 {}:", std::min(recent, n)) : ""));
        std::vector<std::pair<std::string, std::string>> rows;
        for (const auto &s : subs) {
            const std::string pn = s["player"].isObject()
                                       ? s["player"].get("name", "").asString()
                                       : "";
            const std::string ct = s.get("is_fc", false).asBool() ? "FC" : "C";
            const std::string dt = clear_date(s);
            rows.emplace_back(
                dt, fmt::format("  {} ({}) · {}", pn, ct, dt.substr(0, 10)));
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int i = 0; i < recent && i < static_cast<int>(rows.size()); ++i) {
            lines.push_back(rows[i].second);
        }
    }
    send_lines(conf, title, lines);
}

void celeste::cmd_player(const std::string &q, const msg_meta &conf)
{
    std::string query = q;
    const int reqn = trailing_count(query);
    int recent;
    {
        std::lock_guard<std::mutex> lock(mu_);
        recent = reqn > 0 ? reqn : recent_for(conf);
    }
    // Resolve the player via search, then fetch their full verified clears.
    const Json::Value sd = string_to_json(
        http_get(base_url_ + "/api/search/search.php?q=" + url_encode(query)));
    const Json::Value &players = sd["players"];
    if (!players.isArray() || players.empty()) {
        cq_send(conf.p, fmt::format("没找到玩家「{}」。试试 gold ? {}", query, query),
                conf);
        return;
    }
    int pid = -1;
    std::string pname;
    const std::string nq = normalize(query);
    for (const auto &p : players) {
        if (normalize(p.get("name", "").asString()) == nq) {
            pid = p.get("id", 0).asInt();
            pname = p.get("name", "").asString();
            break;
        }
    }
    if (pid < 0) {
        pid = players[0].get("id", 0).asInt();
        pname = players[0].get("name", "").asString();
    }

    const Json::Value subs = string_to_json(http_get(
        base_url_ + fmt::format("/api/player/submissions.php?player_id={}", pid)));
    std::vector<std::pair<std::string, std::string>> rows;
    if (subs.isArray()) {
        for (const auto &s : subs) {
            const Json::Value &c = s["challenge"];
            std::string mn =
                c["map"].isObject() ? c["map"].get("name", "").asString() : "";
            if (mn.empty() && c["campaign"].isObject()) {
                mn = c["campaign"].get("name", "?").asString();
            }
            const std::string tier = c["difficulty"].get("name", "").asString();
            const std::string obj = c["objective"].get("name", "").asString();
            const std::string ct = s.get("is_fc", false).asBool() ? "FC" : "C";
            const std::string dt = clear_date(s);
            rows.emplace_back(
                dt, fmt::format("「{}」· {} · {} ({}) · {}", mn, tier, obj, ct,
                                dt.substr(0, 10)));
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    std::string title = fmt::format("玩家 {}\n{}/player/{}\n记录 {} 条", pname,
                                    base_url_, pid, rows.size());
    if (!rows.empty()) {
        title += fmt::format("，最近 {}:", std::min<int>(recent, rows.size()));
    }
    std::vector<std::string> lines;
    for (int i = 0; i < recent && i < static_cast<int>(rows.size()); ++i) {
        lines.push_back(rows[i].second);
    }
    send_lines(conf, title, lines);
}

void celeste::cmd_search(const std::string &q, const msg_meta &conf)
{
    const Json::Value sd = string_to_json(
        http_get(base_url_ + "/api/search/search.php?q=" + url_encode(q)));
    std::vector<std::string> lines;
    const auto add = [&](const char *label, const Json::Value &arr) {
        if (!arr.isArray() || arr.empty()) {
            return;
        }
        lines.push_back(label);
        int n = 0;
        for (const auto &e : arr) {
            lines.push_back("  " + e.get("name", "").asString());
            if (++n >= 10) {
                break;
            }
        }
    };
    add("地图包(campaign):", sd["campaigns"]);
    add("地图(map):", sd["maps"]);
    add("玩家(player):", sd["players"]);
    if (lines.empty()) {
        cq_send(conf.p, fmt::format("没有匹配「{}」的结果。", q), conf);
        return;
    }
    send_lines(conf, fmt::format("搜索「{}」:", q), lines);
}

void celeste::cmd_tier(const std::string &tier_arg, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "goldberries 数据获取失败，稍后再试。", conf);
        return;
    }
    const std::string a = normalize(tier_arg);
    const bool untiered = (a == "u" || a == "untiered" || a == "wei" || a.empty());
    std::string tier_name;
    if (!untiered) {
        tier_name = "Tier " + tier_arg; // "17" -> "Tier 17"
    }

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (untiered) {
            for (const auto &kv : maps_) {
                if (map_tier_.find(kv.first) == map_tier_.end()) {
                    lines.push_back("  " + kv.second.name);
                }
            }
        }
        else {
            auto it = tier_maps_.find(tier_name);
            if (it != tier_maps_.end()) {
                for (int id : it->second) {
                    lines.push_back("  " + maps_[id].name);
                }
            }
        }
        std::sort(lines.begin(), lines.end());
    }
    if (lines.empty()) {
        cq_send(conf.p,
                fmt::format("{} 没有地图（或档位名不对，如 gold t17 / gold tu）。",
                            untiered ? "未定级" : tier_name),
                conf);
        return;
    }
    send_lines(conf,
               fmt::format("{} 地图 {} 张:", untiered ? "未定级" : tier_name,
                           lines.size()),
               lines);
}

void celeste::cmd_alias(const std::string &arg, const msg_meta &conf)
{
    std::string rest;
    const std::string sub = normalize(first_token(arg, rest));

    if (sub == "list" || sub.empty()) {
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto &kv : aliases_) {
                lines.push_back(fmt::format("  {} -> {}", kv.first, kv.second));
            }
        }
        std::sort(lines.begin(), lines.end());
        if (lines.empty()) {
            cq_send(conf.p, "还没有别名。用法: gold alias add <别名> = <地图>", conf);
            return;
        }
        send_lines(conf, fmt::format("别名 {} 个:", lines.size()), lines);
        return;
    }

    if (!is_op(conf.p, conf.user_id) &&
        !is_group_op(conf.p, conf.group_id, conf.user_id)) {
        cq_send(conf.p, "只有群管理或 bot 管理可以增删别名。", conf);
        return;
    }

    if (sub == "add") {
        const size_t eq = rest.find('=');
        std::string nick = eq == std::string::npos ? "" : trim(rest.substr(0, eq));
        std::string mapn = eq == std::string::npos ? "" : trim(rest.substr(eq + 1));
        if (nick.empty() || mapn.empty()) {
            cq_send(conf.p, "格式: gold alias add <别名> = <地图名>", conf);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            aliases_[normalize(nick)] = mapn;
            save_aliases_unlocked();
        }
        cq_send(conf.p, fmt::format("已添加别名: {} -> {}", nick, mapn), conf);
        return;
    }
    if (sub == "del" || sub == "remove" || sub == "rm") {
        const std::string nick = trim(rest);
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            removed = aliases_.erase(normalize(nick)) > 0;
            if (removed) {
                save_aliases_unlocked();
            }
        }
        cq_send(conf.p,
                removed ? fmt::format("已删除别名: {}", nick) : "没有这个别名。",
                conf);
        return;
    }
    cq_send(conf.p, "用法: gold alias add <别名> = <地图> | del <别名> | list", conf);
}

void celeste::cmd_set(const std::string &arg, const msg_meta &conf)
{
    std::string rest;
    const std::string key = normalize(first_token(arg, rest));
    if (key != "recent") {
        cq_send(conf.p, "用法: gold set recent <n>", conf);
        return;
    }
    if (!is_op(conf.p, conf.user_id) &&
        !is_group_op(conf.p, conf.group_id, conf.user_id)) {
        cq_send(conf.p, "只有群管理或 bot 管理可以设置。", conf);
        return;
    }
    int n = 0;
    try {
        n = std::stoi(trim(rest));
    }
    catch (...) {
        cq_send(conf.p, "用法: gold set recent <n>", conf);
        return;
    }
    n = std::max(1, std::min(50, n));
    {
        std::lock_guard<std::mutex> lock(mu_);
        group_recent_[static_cast<long long>(conf.group_id)] = n;
        save_groups_unlocked();
    }
    cq_send(conf.p, fmt::format("本群显示条数已设为 {}。", n), conf);
}

bool celeste::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    std::string m = trim(message);
    if (!m.empty() && (m[0] == '*' || m[0] == '/')) {
        m = trim(m.substr(1));
    }
    if (m == "gold") {
        return true;
    }
    if (m.rfind("gold", 0) == 0 && m.size() > 4) {
        const char c = m[4];
        return c == ' ' || c == '\t' || c == '?';
    }
    return false;
}

void celeste::process(std::string message, const msg_meta &conf)
{
    std::string m = trim(message);
    if (!m.empty() && (m[0] == '*' || m[0] == '/')) {
        m = trim(m.substr(1));
    }
    std::string rest = (m.size() > 4) ? trim(m.substr(4)) : "";
    if (rest.empty() || rest == "help") {
        cq_send(conf.p, help(), conf);
        return;
    }
    if (rest[0] == '?') {
        cmd_search(trim(rest.substr(1)), conf);
        return;
    }
    std::string arg;
    std::string tok = first_token(rest, arg);
    std::string tl = normalize(tok);
    if (tl == "map") {
        cmd_map(arg, conf);
    }
    else if (tl == "player" || tl == "p") {
        cmd_player(arg, conf);
    }
    else if (tl == "tier") {
        cmd_tier(arg, conf);
    }
    else if (tl == "alias") {
        cmd_alias(arg, conf);
    }
    else if (tl == "set") {
        cmd_set(arg, conf);
    }
    else if (tl == "help") {
        cq_send(conf.p, help(), conf);
    }
    else if (tl.size() >= 2 && tl[0] == 't' &&
             (std::isdigit(static_cast<unsigned char>(tl[1])) || tl == "tu" ||
              tl == "tuntiered")) {
        cmd_tier(tl.substr(1), conf); // "t17"->"17", "tu"->"u"
    }
    else {
        cmd_map(rest, conf); // bare: gold <keyword> => map lookup
    }
}

std::string celeste::help()
{
    return "Celeste 金草莓外榜查询:\nhttps://goldberries.net\n"
           "gold map <关键词> - 查地图难度+最近通关者\n"
           "gold player <名字> - 查玩家+最近金草记录\n"
           "gold ? <模糊词> - 搜索候选地图/玩家\n"
           "gold t<档位> - 列出该档位地图 (如 gold t17, gold tu=未定级)\n"
           "gold ? <模糊词> - 搜索候选\n"
           "gold alias add <别名> = <地图> / del / list - 别名(管理)\n"
           "gold set recent <n> - 本群显示条数(管理)\n"
           "gold <关键词> - 等同 gold map\n"
           "查更多记录: 名字后加数字, 如 gold player viddie 10 (或 10条)";
}

bool celeste::reload(const msg_meta &conf)
{
    (void)conf;
    std::lock_guard<std::mutex> lock(mu_);
    load_config_unlocked();
    load_aliases_unlocked();
    load_groups_unlocked();
    cache_ts_ = 0; // force refetch on next query
    return true;
}

DECLARE_FACTORY_FUNCTIONS(celeste)
