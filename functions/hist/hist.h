#pragma once

#include "processable.h"

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// CN Hist plugin: celemiao.com hardest-map tier list (star rating + subtier).
// The whole map list (208) is fetched once; players (paginated) are cached in
// full too, so lookups run locally. Per-map/per-player clears fetched on demand.
class hist : public processable {
private:
    struct MapInfo {
        int id = 0;
        std::string slug;
        std::string name;
        std::string author;
        int stars = 0;
        std::string sub_tier; // "upper" / "lower" / ""
        int cleared_count = 0;
        bool is_quality = false;
    };
    struct PlayerInfo {
        int id = 0;
        std::string username;
        int cleared_count = 0;
        int total_stars = 0;
        int max_stars = 0;
    };

    mutable std::mutex mu_;
    std::time_t cache_ts_ = 0;
    int cache_ttl_sec_ = 3600;
    int default_recent_ = 3;
    std::string base_url_ = "https://bbs.celemiao.com";

    std::unordered_map<int, MapInfo> maps_;                 // id -> map
    std::vector<std::pair<std::string, int>> map_name_idx_; // (normalized name, id)
    std::map<int, std::vector<int>> star_maps_;             // stars -> map ids
    std::unordered_map<int, PlayerInfo> players_;           // id -> player
    std::vector<std::pair<std::string, int>> player_name_idx_; // (norm name, id)

    // global aliases (community-shared): normalized nickname -> canonical map name
    std::unordered_map<std::string, std::string> aliases_;
    // per-group recent-count override (group_id -> n)
    std::unordered_map<long long, int> group_recent_;

    void load_config_unlocked();
    void load_aliases_unlocked();
    void save_aliases_unlocked() const;
    void load_groups_unlocked();
    void save_groups_unlocked() const;
    int recent_for(const msg_meta &conf) const;
    bool ensure_cache();
    bool fetch_and_build_unlocked();
    static std::string normalize(const std::string &s);
    static std::string http_get(const std::string &url);
    static std::string difficulty_label(int stars, const std::string &sub_tier);

    std::vector<int> resolve_maps(const std::string &q) const; // alias-aware
    int resolve_player(const std::string &q) const;            // id or -1

    void cmd_map(const std::string &q, const msg_meta &conf);
    void cmd_player(const std::string &q, const msg_meta &conf);
    void cmd_search(const std::string &q, const msg_meta &conf);
    void cmd_tier(int stars, const std::string &want_sub, const msg_meta &conf);
    void cmd_alias(const std::string &arg, const msg_meta &conf);
    void cmd_set(const std::string &arg, const msg_meta &conf);

    // send `title` + `lines`; fold into a merged-forward message when long
    void send_lines(const msg_meta &conf, const std::string &title,
                    const std::vector<std::string> &lines);

public:
    hist();
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    bool reload(const msg_meta &conf) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
