#pragma once

#include "processable.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class bottle : public processable {
private:
    struct bottle_item {
        std::string text;
        int64_t created_at = 0;
        userid_t sender_user_id = 0;
        groupid_t sender_group_id = 0;
        std::string sender_message_type;
    };
    struct cd_config {
        int drop_cd_sec = 60;
        int pick_cd_sec = 60;
    };

    mutable std::mutex mutex_;
    std::vector<bottle_item> bottles_;

    const std::string storage_path_ =
        bot_config_path("features/bottle/bottles.json");

    static constexpr size_t kMaxBottleTextLen = 500;
    static constexpr size_t kMaxBottleCount = 5000;
    static constexpr int64_t kDedupIntervalSec = 600;

    int64_t last_dedup_at_ = 0;
    size_t mutations_since_dedup_ = 0;

    std::unordered_map<groupid_t, cd_config> group_cd_;

    std::unordered_map<groupid_t, std::unordered_map<userid_t, int64_t>>
        last_drop_time_;

    std::unordered_map<groupid_t, std::unordered_map<userid_t, int64_t>>
        last_pick_time_;

    std::unordered_map<groupid_t, std::unordered_map<userid_t, int64_t>>
        last_cd_notify_time_;

    void load_unlocked();
    void save_unlocked() const;
    void dedup_bottles_unlocked();
    void maybe_dedup_unlocked(int64_t now_sec, bool force = false);
    bool check_cd_and_notify(groupid_t gid, userid_t uid, int64_t now,
                             int64_t last_time, int cd_sec,
                             const msg_meta &conf);
    cd_config &get_cd_config_unlocked(groupid_t gid);

public:
    bottle();

    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    bool reload(const msg_meta &conf) override;

    std::string help() override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
