#pragma once

#include "processable.h"

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Celeste community plugin (v1): goldberries.net (global goldlist), query-only.
// Fetches the top-golden-list once and caches it; all queries run off the cache.
class celeste : public processable {
private:
    struct MapInfo {
        int id = 0;
        std::string name;
        int campaign_id = 0;
        bool is_archived = false;
    };

    mutable std::mutex mu_;
    std::time_t cache_ts_ = 0;
    int cache_ttl_sec_ = 3600;
    int default_recent_ = 3;
    std::string base_url_ = "https://goldberries.net";

    // Structure indices built from top-golden-list.php (tiers + name resolution).
    // Per-map clearers and per-player clears are fetched on demand from the map /
    // player-submission detail endpoints — the golden-list only carries one
    // representative submission per challenge.
    std::unordered_map<int, MapInfo> maps_;                 // map id -> info
    std::unordered_map<int, std::string> campaigns_;        // campaign id -> name
    std::vector<std::pair<std::string, int>> map_name_idx_; // (normalized name, map id)
    // tier name -> challenge labels (map name + objective note); a map can
    // appear in several tiers via different challenges.
    std::map<std::string, std::vector<std::string>> tier_maps_;

    // global aliases (community-shared): normalized nickname -> canonical map name
    std::unordered_map<std::string, std::string> aliases_;
    // per-group recent-count override (group_id -> n)
    std::unordered_map<long long, int> group_recent_;

    void load_config_unlocked();
    void load_aliases_unlocked();
    void save_aliases_unlocked() const;
    void load_groups_unlocked();
    void save_groups_unlocked() const;
    int recent_for(const msg_meta &conf) const; // per-group override or default
    void cmd_alias(const std::string &arg, const msg_meta &conf);
    void cmd_set(const std::string &arg, const msg_meta &conf);
    bool ensure_cache();               // fetch+build if empty/stale (locks)
    bool fetch_and_build_unlocked();   // does the HTTP + index build
    static std::string normalize(const std::string &s);
    static std::string http_get(const std::string &url);

    // resolution (return candidate map ids ranked, alias-aware)
    std::vector<int> resolve_maps(const std::string &q) const;

    // command handlers — send directly (support folding)
    void cmd_map(const std::string &q, const msg_meta &conf);
    void cmd_player(const std::string &q, const msg_meta &conf);
    void cmd_search(const std::string &q, const msg_meta &conf);
    void cmd_tier(const std::string &tier_arg, const msg_meta &conf);

    // send `title` + `lines`; fold into a merged-forward message when > threshold
    void send_lines(const msg_meta &conf, const std::string &title,
                    const std::vector<std::string> &lines);

public:
    celeste();
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    bool reload(const msg_meta &conf) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
