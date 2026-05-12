#pragma once

#include "processable.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

struct rua_item {
    std::string name;
    std::string description;
    std::string image;
    int favor = 0;
};

struct favor_text_level {
    int min = 0;
    std::string hearts;
    std::vector<std::string> texts;
};

class rua : public processable {
private:
    std::vector<rua_item> items_;
    std::vector<favor_text_level> favor_text_levels_;
    std::map<userid_t, int64_t> favor_by_user_;
    std::map<userid_t, int> daily_rua_count_by_user_;
    std::string daily_rua_date_;
    std::map<userid_t, rua_item> pending_add_by_user_;
    std::mutex mutex_;
    bool random_favor_mode_ = false;
    std::string config_dir_ = bot_config_path("");
    std::string resource_dir_ = bot_resource_path("");

    std::string rua_config_path() const;
    std::string rua_state_path() const;
    std::filesystem::path rua_resource_dir() const;
    void sync_dirs_from_bot(const bot *p);
    void load_config();
    void load_favor_text_config();
    void save_config_unlocked() const;
    void recompute_random_mode_unlocked();
    void load_favor_storage();
    void save_favor_storage_unlocked() const;
    void load_daily_limit_storage();
    void save_daily_limit_storage_unlocked() const;
    void reset_daily_limit_if_needed_unlocked();
    int pick_favor_delta(const rua_item &item) const;
    const favor_text_level &pick_favor_level(int64_t total) const;
    std::string pick_random_alt(const std::vector<std::string> &alts,
                                const std::string &fallback) const;
    std::string format_query_favor_text(int64_t total) const;
    std::string format_draw_favor_text(int64_t total) const;
    bool is_admin(const msg_meta &conf) const;
    int find_item_index_by_name(const std::string &name) const;
    std::string parse_image_url(std::string message,
                                const msg_meta &conf) const;
    std::string save_image_from_message(const std::string &message,
                                        const msg_meta &conf) const;
    std::pair<size_t, size_t> target_image_size_from_kunkun() const;
    void cleanup_orphan_images_unlocked();
    std::string normalized_image_file(const std::string &raw_file,
                                      bool refresh) const;
    std::string resolve_image_file(const std::string &raw_file,
                                   bool refresh_normalized = false) const;

public:
    rua();
    void process(std::string message, const msg_meta &conf);
    bool check(std::string message, const msg_meta &conf);
    bool reload(const msg_meta &conf) override;
    std::string help();
    std::string help(const msg_meta &conf, help_level_t level);
};

DECLARE_FACTORY_FUNCTIONS_HEADER
