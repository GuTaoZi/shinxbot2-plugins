#include "hist.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <fmt/format.h>
#include <mutex>
#include <sstream>

namespace {
const std::string kConfig = "features/hist/hist.json";
const std::string kAliases = "features/hist/aliases.json";
const std::string kGroups = "features/hist/groups.json";

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

// Escape CQ-code specials so literal text (e.g. map names with '[') survives
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
// single-token query.
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

// Normalize a subtier word: upper/u -> "upper", lower/l -> "lower", else "".
std::string norm_subtier(const std::string &s)
{
    std::string t;
    for (char c : s) {
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (t == "upper" || t == "u" || t == "up") {
        return "upper";
    }
    if (t == "lower" || t == "l" || t == "low") {
        return "lower";
    }
    return "";
}
} // namespace

hist::hist()
{
    load_config_unlocked();
    load_aliases_unlocked();
    load_groups_unlocked();
}

void hist::load_config_unlocked()
{
    Json::Value j = string_to_json(readfile(bot_config_path(nullptr, kConfig), "{}"));
    if (j.isObject()) {
        base_url_ = j.get("base_url", base_url_).asString();
        cache_ttl_sec_ = j.get("cache_ttl_sec", cache_ttl_sec_).asInt();
        default_recent_ = std::max(1, j.get("default_recent", default_recent_).asInt());
    }
}

void hist::load_aliases_unlocked()
{
    aliases_.clear();
    Json::Value j = string_to_json(readfile(bot_config_path(nullptr, kAliases), "{}"));
    if (j.isObject()) {
        for (const auto &k : j.getMemberNames()) {
            aliases_[normalize(k)] = j[k].asString();
        }
    }
}

void hist::save_aliases_unlocked() const
{
    Json::Value j(Json::objectValue);
    for (const auto &kv : aliases_) {
        j[kv.first] = kv.second;
    }
    writefile(bot_config_path(nullptr, kAliases), j.toStyledString(), false);
}

void hist::load_groups_unlocked()
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

void hist::save_groups_unlocked() const
{
    Json::Value j(Json::objectValue);
    for (const auto &kv : group_recent_) {
        j[std::to_string(kv.first)] = kv.second;
    }
    writefile(bot_config_path(nullptr, kGroups), j.toStyledString(), false);
}

int hist::recent_for(const msg_meta &conf) const
{
    auto it = group_recent_.find(static_cast<long long>(conf.group_id));
    return it != group_recent_.end() ? it->second : default_recent_;
}

std::string hist::http_get(const std::string &url)
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
    curl_easy_setopt(h, CURLOPT_USERAGENT, "shinxbot-hist/1.0");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    if (curl_easy_perform(h) != CURLE_OK) {
        return "";
    }
    return out;
}

std::string hist::normalize(const std::string &s)
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

std::string hist::difficulty_label(int stars, const std::string &sub_tier)
{
    const char *arrow =
        sub_tier == "upper" ? "↑" : (sub_tier == "lower" ? "↓" : "");
    return fmt::format("{}★{}", stars, arrow);
}

bool hist::ensure_cache()
{
    std::lock_guard<std::mutex> lock(mu_);
    const std::time_t now = std::time(nullptr);
    if (!maps_.empty() && now - cache_ts_ < cache_ttl_sec_) {
        return true;
    }
    return fetch_and_build_unlocked();
}

bool hist::fetch_and_build_unlocked()
{
    const Json::Value mroot = string_to_json(http_get(base_url_ + "/api/hist/maps"));
    if (!mroot.isObject() || !mroot["maps"].isArray()) {
        return false;
    }

    maps_.clear();
    map_name_idx_.clear();
    star_maps_.clear();
    players_.clear();
    player_name_idx_.clear();

    for (const auto &m : mroot["maps"]) {
        MapInfo mi;
        mi.id = m.get("id", 0).asInt();
        mi.slug = m.get("slug", "").asString();
        mi.name = m.get("name", "").asString();
        mi.author = m.get("author", "").asString();
        mi.stars = m.get("stars", 0).asInt();
        mi.sub_tier = m["subTier"].isString() ? m["subTier"].asString() : "";
        mi.cleared_count = m.get("clearedCount", 0).asInt();
        mi.is_quality = m.get("isQuality", false).asBool();
        if (mi.id == 0) {
            continue;
        }
        maps_[mi.id] = mi;
        map_name_idx_.emplace_back(normalize(mi.name), mi.id);
        star_maps_[mi.stars].push_back(mi.id);
    }

    // Players are paginated; pull every page so lookups + ranking run locally.
    Json::Value p1 = string_to_json(http_get(base_url_ + "/api/hist/players?page=1"));
    const int total_pages = p1["pagination"].get("totalPages", 1).asInt();
    for (int page = 1; page <= total_pages; ++page) {
        const Json::Value &pr =
            (page == 1) ? p1
                        : string_to_json(http_get(
                              base_url_ + fmt::format("/api/hist/players?page={}", page)));
        for (const auto &p : pr["players"]) {
            PlayerInfo pi;
            pi.id = p.get("id", 0).asInt();
            pi.username = p.get("username", "").asString();
            pi.cleared_count = p.get("clearedCount", 0).asInt();
            pi.total_stars = p.get("totalStars", 0).asInt();
            pi.max_stars = p.get("maxStars", 0).asInt();
            if (pi.id == 0) {
                continue;
            }
            players_[pi.id] = pi;
            player_name_idx_.emplace_back(normalize(pi.username), pi.id);
        }
    }

    cache_ts_ = std::time(nullptr);
    return true;
}

std::vector<int> hist::resolve_maps(const std::string &q) const
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

int hist::resolve_player(const std::string &q) const
{
    const std::string nq = normalize(q);
    if (nq.empty()) {
        return -1;
    }
    int prefix = -1, sub = -1;
    for (const auto &pr : player_name_idx_) {
        const std::string &n = pr.first;
        if (n == nq) {
            return pr.second; // exact wins
        }
        if (prefix < 0 && n.rfind(nq, 0) == 0) {
            prefix = pr.second;
        }
        else if (sub < 0 && n.find(nq) != std::string::npos) {
            sub = pr.second;
        }
    }
    return prefix >= 0 ? prefix : sub;
}

void hist::send_lines(const msg_meta &conf, const std::string &title,
                      const std::vector<std::string> &lines)
{
    std::ostringstream full;
    full << title;
    for (const auto &l : lines) {
        full << "\n" << l;
    }

    // Keep short replies inline; fold long ones into a merged-forward message.
    constexpr size_t kFoldOver = 10;
    if (lines.size() <= kFoldOver) {
        cq_send(conf.p, full.str(), conf);
        return;
    }

    // Fold into a QQ merged-forward message, chunked into nodes — same approach
    // as the 美图 plugin, works in both group and private chats.
    Json::Value messages(Json::arrayValue);
    const std::string uin = std::to_string(conf.p->get_botqq());
    auto node = [&](const std::string &content) {
        Json::Value n(Json::objectValue), d(Json::objectValue);
        n["type"] = "node";
        d["name"] = "Hist";
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

void hist::cmd_map(const std::string &q, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "Hist 数据获取失败，稍后再试。", conf);
        return;
    }
    std::string query = q;
    const int reqn = trailing_count(query);
    std::vector<int> ids;
    MapInfo mi;
    int recent = default_recent_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        recent = reqn > 0 ? reqn : recent_for(conf);
        ids = resolve_maps(query);
        if (ids.size() == 1) {
            mi = maps_[ids[0]];
        }
    }

    if (ids.empty()) {
        cq_send(conf.p, fmt::format("没找到地图「{}」。试试 hist ? {}", query, query),
                conf);
        return;
    }
    if (ids.size() > 1) {
        std::vector<std::string> cand;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (int id : ids) {
                const MapInfo &c = maps_[id];
                cand.push_back(fmt::format("{}. {} · {}", cand.size() + 1, c.name,
                                           difficulty_label(c.stars, c.sub_tier)));
            }
        }
        send_lines(conf, fmt::format("找到多个地图匹配「{}」，请更精确：", query), cand);
        return;
    }

    // Runs are paginated 20/page, oldest-first — so the freshest clears live on
    // the LAST page(s). Pull only the tail pages needed to cover `recent`.
    auto fetch_page = [&](int page) {
        return string_to_json(
            http_get(base_url_ + fmt::format("/api/hist/maps/{}?page={}", mi.slug, page)));
    };
    const Json::Value first = fetch_page(1);
    const Json::Value &pg = first["runsPagination"];
    const int total_pages = std::max(1, pg.get("totalPages", 1).asInt());
    const int per_page = std::max(1, pg.get("perPage", 20).asInt());
    const int need_pages = std::min(total_pages, (recent + per_page - 1) / per_page);
    const int start_page = total_pages - need_pages + 1;

    std::vector<std::pair<std::string, std::string>> rows;
    for (int p = start_page; p <= total_pages; ++p) {
        const Json::Value d = (p == 1) ? first : fetch_page(p);
        for (const auto &r : d["runs"]) {
            const std::string dt = r.get("completedAt", "").asString();
            rows.emplace_back(dt, fmt::format("  {} · {}",
                                              r.get("username", "").asString(),
                                              dt.substr(0, 10)));
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    const std::string title =
        fmt::format("「{}」 ({})\n{}/hist/maps/{}", mi.name, mi.author, base_url_, mi.slug);
    std::vector<std::string> lines;
    lines.push_back(fmt::format(
        "难度: {}{} · 通关 {} 人{}", difficulty_label(mi.stars, mi.sub_tier),
        mi.is_quality ? " · 品质" : "", mi.cleared_count,
        rows.empty() ? "" : fmt::format("，最近 {}:", std::min<int>(recent, rows.size()))));
    for (int i = 0; i < recent && i < static_cast<int>(rows.size()); ++i) {
        lines.push_back(rows[i].second);
    }
    send_lines(conf, title, lines);
}

void hist::cmd_player(const std::string &q, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "Hist 数据获取失败，稍后再试。", conf);
        return;
    }
    std::string query = q;
    const int reqn = trailing_count(query);
    PlayerInfo pi;
    int recent = default_recent_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        recent = reqn > 0 ? reqn : recent_for(conf);
        const int id = resolve_player(query);
        if (id < 0) {
            cq_send(conf.p,
                    fmt::format("没找到玩家「{}」。试试 hist ? {}", query, query), conf);
            return;
        }
        pi = players_[id];
    }

    const Json::Value d = string_to_json(
        http_get(base_url_ + fmt::format("/api/hist/players/{}", pi.id)));
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto &m : d["maps"]) {
        const std::string dt = m.get("completedAt", "").asString();
        const std::string sub = m["subTier"].isString() ? m["subTier"].asString() : "";
        const bool gb = m.get("tagGoldenBerry", false).asBool();
        rows.emplace_back(
            dt, fmt::format("「{}」· {} · {}{}", m.get("name", "").asString(),
                            difficulty_label(m.get("stars", 0).asInt(), sub),
                            dt.substr(0, 10), gb ? " 金草" : ""));
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    const std::string title = fmt::format("玩家 {}\n{}/hist/players/{}", pi.username,
                                           base_url_, pi.id);
    std::vector<std::string> lines;
    lines.push_back(fmt::format(
        "通关 {} 张 · 总星 {} · 最高 {}★{}", pi.cleared_count, pi.total_stars,
        pi.max_stars,
        rows.empty() ? "" : fmt::format("，最近 {}:", std::min<int>(recent, rows.size()))));
    for (int i = 0; i < recent && i < static_cast<int>(rows.size()); ++i) {
        lines.push_back(rows[i].second);
    }
    send_lines(conf, title, lines);
}

void hist::cmd_tier(const std::string &arg, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "Hist 数据获取失败，稍后再试。", conf);
        return;
    }
    std::string rest;
    const std::string stars_tok = first_token(arg, rest);
    int stars = -1;
    try {
        stars = std::stoi(stars_tok);
    }
    catch (...) {
        cq_send(conf.p, "用法: hist t<星数> [upper|lower]，如 hist t5 upper", conf);
        return;
    }
    const std::string want_sub = norm_subtier(rest);

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = star_maps_.find(stars);
        if (it != star_maps_.end()) {
            std::vector<int> ids = it->second;
            std::sort(ids.begin(), ids.end(), [this](int a, int b) {
                if (maps_[a].sub_tier != maps_[b].sub_tier) {
                    return maps_[a].sub_tier > maps_[b].sub_tier; // upper before lower
                }
                return maps_[a].name < maps_[b].name;
            });
            for (int id : ids) {
                const MapInfo &m = maps_[id];
                if (!want_sub.empty() && m.sub_tier != want_sub) {
                    continue;
                }
                lines.push_back(
                    want_sub.empty() && !m.sub_tier.empty()
                        ? fmt::format("  {} {}", m.name,
                                      m.sub_tier == "upper" ? "↑" : "↓")
                        : "  " + m.name);
            }
        }
    }
    const std::string label = difficulty_label(stars, want_sub);
    if (lines.empty()) {
        cq_send(conf.p, fmt::format("{} 没有地图。", label), conf);
        return;
    }
    send_lines(conf, fmt::format("Hist {} 地图 {} 张:", label, lines.size()), lines);
}

void hist::cmd_search(const std::string &q, const msg_meta &conf)
{
    if (!ensure_cache()) {
        cq_send(conf.p, "Hist 数据获取失败，稍后再试。", conf);
        return;
    }
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<int> ids = resolve_maps(q);
        if (!ids.empty()) {
            lines.push_back("地图:");
            for (int id : ids) {
                const MapInfo &m = maps_[id];
                lines.push_back(
                    fmt::format("  {} · {}", m.name, difficulty_label(m.stars, m.sub_tier)));
            }
        }
        const std::string nq = normalize(q);
        std::vector<std::string> players;
        if (!nq.empty()) {
            for (const auto &pr : player_name_idx_) {
                if (pr.first.find(nq) != std::string::npos) {
                    players.push_back("  " + players_[pr.second].username);
                    if (players.size() >= 10) {
                        break;
                    }
                }
            }
        }
        if (!players.empty()) {
            lines.push_back("玩家:");
            for (auto &p : players) {
                lines.push_back(p);
            }
        }
    }
    if (lines.empty()) {
        cq_send(conf.p, fmt::format("没有匹配「{}」的地图或玩家。", q), conf);
        return;
    }
    send_lines(conf, fmt::format("搜索「{}」:", q), lines);
}

void hist::cmd_alias(const std::string &arg, const msg_meta &conf)
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
            cq_send(conf.p, "还没有别名。用法: hist alias add <别名> = <地图>", conf);
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
            cq_send(conf.p, "格式: hist alias add <别名> = <地图名>", conf);
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
                removed ? fmt::format("已删除别名: {}", nick) : "没有这个别名。", conf);
        return;
    }
    cq_send(conf.p, "用法: hist alias add <别名> = <地图> | del <别名> | list", conf);
}

void hist::cmd_set(const std::string &arg, const msg_meta &conf)
{
    std::string rest;
    const std::string key = normalize(first_token(arg, rest));
    if (key != "recent") {
        cq_send(conf.p, "用法: hist set recent <n>", conf);
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
        cq_send(conf.p, "用法: hist set recent <n>", conf);
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

bool hist::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    std::string m = trim(message);
    if (!m.empty() && (m[0] == '*' || m[0] == '/')) {
        m = trim(m.substr(1));
    }
    if (m == "hist") {
        return true;
    }
    if (m.rfind("hist", 0) == 0 && m.size() > 4) {
        const char c = m[4];
        return c == ' ' || c == '\t' || c == '?';
    }
    return false;
}

void hist::process(std::string message, const msg_meta &conf)
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
             std::isdigit(static_cast<unsigned char>(tl[1]))) {
        // "t5" / "t5 upper" -> tier 5 (subtier from arg)
        cmd_tier(tok.substr(1) + (arg.empty() ? "" : " " + arg), conf);
    }
    else {
        cmd_map(rest, conf); // bare: hist <keyword> => map lookup
    }
}

std::string hist::help()
{
    return "CN Hist 查询:\nhttps://bbs.celemiao.com/hist\n"
           "hist map <关键词> - 查地图难度+最近通关者\n"
           "hist player <名字> - 查玩家统计+最近通关\n"
           "hist t<星数> [upper|lower] - 列出该难度地图 (如 hist t5 upper)\n"
           "hist ? <模糊词> - 搜索候选地图/玩家\n"
           "hist alias add <别名> = <地图> / del / list - 别名(管理)\n"
           "hist set recent <n> - 本群显示条数(管理)\n"
           "hist <关键词> - 等同 hist map\n"
           "查更多记录: 名字后加数字, 如 hist player Kuro 10 (或 10条)";
}

bool hist::reload(const msg_meta &conf)
{
    (void)conf;
    std::lock_guard<std::mutex> lock(mu_);
    load_config_unlocked();
    load_aliases_unlocked();
    load_groups_unlocked();
    cache_ts_ = 0; // force refetch on next query
    return true;
}

DECLARE_FACTORY_FUNCTIONS(hist)
