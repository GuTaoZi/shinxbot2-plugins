#pragma once

#include "processable.h"

#include <atomic>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class bili : public processable {
private:
    static constexpr int kDefaultPollIntervalSec = 90;

    struct name_cache_t {
        std::string name;
        std::time_t expire_at = 0;
    };

    struct up_cache_t {
        bool initialized = false;
        std::string name;
        std::string last_dynamic_id;
        std::string last_dynamic_text;
        int64_t last_dynamic_pub_ts = 0;
        std::string last_video_bvid;
        std::string last_video_title;
        int64_t last_video_pub_ts = 0;
        int last_live_status = -1;
        uint64_t last_live_room_id = 0;
        std::string last_live_title;
    };

    struct up_snapshot_t {
        bool has_any = false;
        std::string name;
        std::string avatar;
        userid_t canonical_uid = 0;
        bool has_profile = false;
        int64_t profile_archive_count = -1;
        int64_t profile_follower = -1;

        bool has_dynamic = false;
        std::string dynamic_id;
        std::string dynamic_text;
        std::string dynamic_cover;
        int64_t dynamic_pub_ts = 0;
        std::string dynamic_reason;

        bool has_video = false;
        std::string video_bvid;
        std::string video_title;
        std::string video_cover;
        int64_t video_pub_ts = 0;
        std::string video_reason;

        bool has_live = false;
        int live_status = -1;
        uint64_t live_room_id = 0;
        std::string live_title;
        std::string live_cover;
    };

    mutable std::mutex mutex_;
    std::unordered_map<groupid_t, std::set<userid_t>> group_subscriptions_;
    std::set<groupid_t> group_member_manage_open_;
    std::unordered_map<userid_t, up_cache_t> up_cache_;
    mutable std::unordered_map<userid_t, name_cache_t> list_name_cache_;
    std::string cookie_override_;

    static constexpr int kCleanupIntervalSec = 3600; // prune orphans hourly
    // Video (WBI arc/search) is heavily rate-limited, so each poll cycle only
    // checks this many UIDs' videos, round-robin, to stay under risk control.
    static constexpr size_t kVideoPerPoll = 8;

    int poll_interval_sec_ = kDefaultPollIntervalSec;
    std::time_t next_poll_ts_ = 0;
    std::time_t next_cleanup_ts_ = 0;
    size_t video_poll_cursor_ = 0; // round-robin index into the poll UID list
    std::time_t last_poll_callback_ts_ = 0;
    std::time_t last_poll_run_ts_ = 0;
    uint64_t poll_callback_count_ = 0;
    uint64_t poll_run_count_ = 0;

    void load_config_unlocked();
    void save_config_unlocked() const;
    void load_cache_unlocked();
    void save_cache_unlocked() const;
    void load_cookie_secret_unlocked();
    void save_cookie_secret_unlocked() const;

    std::string config_path() const;
    std::string cache_path() const;

    bool can_manage_group(const msg_meta &conf) const;
    bool can_modify_subscriptions(const msg_meta &conf) const;
    bool is_group_member_manage_open(groupid_t gid) const;
    std::string build_help_for_context(const msg_meta &conf) const;
    bool parse_group_and_uids(
        const msg_meta &conf, const std::string &args, groupid_t &target_group,
        std::vector<userid_t> &uids, bool allow_empty_uid_list,
        std::vector<std::string> *failed_tokens = nullptr) const;

    bool resolve_target_to_uid(const std::string &token, userid_t &uid_out,
                               std::string &err,
                               bool allow_room_id_resolution = false) const;
    std::vector<userid_t>
    parse_uid_list(const std::string &args,
                   std::vector<std::string> *failed_tokens = nullptr) const;
    static bool is_uid_token(const std::string &token);
    static std::string compact_text(const std::string &raw, size_t max_len);

    up_snapshot_t fetch_snapshot(userid_t uid) const;
    // want_video: fetch the latest video via the (rate-limited) WBI arc/search.
    // The poll passes true for only a rotating subset of UIDs each cycle so we
    // stay under bilibili's risk-control threshold; live is always fetched.
    up_snapshot_t fetch_snapshot_for_poll(userid_t uid, bool want_video) const;
    bool resolve_uid_by_room_id(userid_t room_id, up_snapshot_t &snapshot,
                                userid_t &uid_out) const;
    bool fetch_live_snapshot(userid_t uid, up_snapshot_t &snapshot) const;
    bool fetch_live_snapshot_via_master(userid_t uid,
                                        up_snapshot_t &snapshot) const;
    bool fetch_profile_snapshot(userid_t uid, up_snapshot_t &snapshot) const;
    bool fetch_video_snapshot(userid_t uid, up_snapshot_t &snapshot) const;
    bool fetch_dynamic_snapshot(userid_t uid, up_snapshot_t &snapshot) const;

    std::string build_dynamic_push_message(userid_t uid,
                                           const up_snapshot_t &snap) const;
    std::string build_video_push_message(userid_t uid,
                                         const up_snapshot_t &snap) const;
    std::string build_live_on_push_message(userid_t uid,
                                           const up_snapshot_t &snap) const;
    std::string build_live_off_push_message(userid_t uid,
                                            const up_snapshot_t &snap) const;

    std::string query_one(userid_t uid) const;
    std::string list_group_subscriptions(groupid_t gid) const;
    std::string build_live_now_message(userid_t uid,
                                       const up_snapshot_t &snap) const;
    bool send_list_group_subscriptions_forward(groupid_t gid,
                                               const msg_meta &conf) const;
    bool send_live_now_forward(groupid_t gid, const msg_meta &conf) const;
    // Sends a bili_decode video-info message: inline if short, else folded
    // into a merged-forward (group or private) so a long 简介 doesn't spam
    // the chat with a giant single message.
    void send_decoded_video(const std::string &out, const msg_meta &conf) const;

    void handle_poll(bot *p);

    // Cleanup: drop cache entries for UIDs no longer subscribed by any group,
    // and (using the bot's live group list) subscriptions for groups the bot
    // has left/been removed from. Runs periodically from handle_poll.
    size_t prune_orphan_cache_unlocked();
    void run_maintenance(bot *p);

public:
    bili();
    ~bili() override;

    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;

    bool reload(const msg_meta &conf) override;
    std::string help() override;
    std::string help(const msg_meta &conf, help_level_t level) override;

    void
    set_callback(std::function<void(std::function<void(bot *p)>)> f) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
