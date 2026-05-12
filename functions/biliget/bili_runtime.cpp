#include "bili.h"
#include "bili_utils.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <fmt/format.h>
#include <thread>
#include <vector>

namespace {

using biliget_utils::is_dynamic_id_newer_than;
using biliget_utils::is_fresh_publish_ts;
using biliget_utils::join_tokens;
using biliget_utils::send_group_msg_checked;

} // namespace

void biliget::handle_poll(bot *p)
{
    if (p == nullptr) {
        return;
    }

    std::time_t now = std::time(nullptr);
    std::unordered_map<userid_t, std::vector<groupid_t>> uid_to_groups;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++poll_callback_count_;
        last_poll_callback_ts_ = now;
        if (now < next_poll_ts_) {
            return;
        }

        ++poll_run_count_;
        last_poll_run_ts_ = now;
        next_poll_ts_ = now + poll_interval_sec_;

        for (const auto &it : group_subscriptions_) {
            for (userid_t uid : it.second) {
                uid_to_groups[uid].push_back(it.first);
            }
        }
    }

    if (uid_to_groups.empty()) {
        return;
    }

    std::vector<userid_t> uid_list;
    uid_list.reserve(uid_to_groups.size());
    for (const auto &entry : uid_to_groups) {
        uid_list.push_back(entry.first);
    }

    std::vector<up_snapshot_t> snaps(uid_list.size());
    std::vector<bool> snap_ok(uid_list.size(), false);

    std::atomic<size_t> cursor{0};
    const unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    const size_t worker_count =
        std::min<size_t>(uid_list.size(), std::min<size_t>(hc, 8));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t i = cursor.fetch_add(1);
                if (i >= uid_list.size()) {
                    break;
                }

                const userid_t uid = uid_list[i];
                try {
                    snaps[i] = fetch_snapshot_for_poll(uid);
                    snap_ok[i] = true;
                }
                catch (...) {
                    // Ignore per-user snapshot exceptions in poll loop.
                }
            }
        });
    }
    for (auto &t : workers) {
        t.join();
    }

    bool cache_dirty = false;

    for (size_t i = 0; i < uid_list.size(); ++i) {
        if (!snap_ok[i]) {
            continue;
        }
        const userid_t uid = uid_list[i];
        const up_snapshot_t &snap = snaps[i];
        if (!snap.has_any) {
            continue;
        }

        bool push_dynamic = false;
        bool push_video = false;
        bool push_live_on = false;
        bool push_live_off = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            up_cache_t &cache = up_cache_[uid];

            if (!snap.name.empty()) {
                cache.name = snap.name;
            }

            if (!cache.initialized) {
                if (snap.has_dynamic) {
                    cache.last_dynamic_id = snap.dynamic_id;
                    cache.last_dynamic_text = snap.dynamic_text;
                    cache.last_dynamic_pub_ts = snap.dynamic_pub_ts;
                }
                if (snap.has_video) {
                    cache.last_video_bvid = snap.video_bvid;
                    cache.last_video_title = snap.video_title;
                    cache.last_video_pub_ts = snap.video_pub_ts;
                }
                if (snap.has_live) {
                    cache.last_live_status = snap.live_status;
                    cache.last_live_room_id = snap.live_room_id;
                    cache.last_live_title = snap.live_title;
                }
                cache.initialized = true;
                cache_dirty = true;
                continue;
            }

            if (snap.has_dynamic && !snap.dynamic_id.empty() &&
                snap.dynamic_id != cache.last_dynamic_id) {
                bool stale_dynamic = false;
                if (cache.last_dynamic_pub_ts > 0) {
                    if (snap.dynamic_pub_ts <= 0 ||
                        snap.dynamic_pub_ts < cache.last_dynamic_pub_ts) {
                        stale_dynamic = true;
                    }
                }
                if (!stale_dynamic) {
                    if (!is_dynamic_id_newer_than(snap.dynamic_id,
                                                  cache.last_dynamic_id)) {
                        stale_dynamic = true;
                    }
                }
                if (!stale_dynamic) {
                    const bool fresh_dynamic =
                        is_fresh_publish_ts(snap.dynamic_pub_ts, now);
                    push_dynamic =
                        !cache.last_dynamic_id.empty() && fresh_dynamic;
                    cache.last_dynamic_id = snap.dynamic_id;
                    cache.last_dynamic_text = snap.dynamic_text;
                    if (snap.dynamic_pub_ts > 0) {
                        cache.last_dynamic_pub_ts = snap.dynamic_pub_ts;
                    }
                    cache_dirty = true;
                }
            }

            if (snap.has_video && !snap.video_bvid.empty() &&
                snap.video_bvid != cache.last_video_bvid) {
                bool stale_video = false;
                if (cache.last_video_pub_ts > 0) {
                    if (snap.video_pub_ts <= 0 ||
                        snap.video_pub_ts < cache.last_video_pub_ts) {
                        stale_video = true;
                    }
                }
                if (!stale_video) {
                    const bool fresh_video =
                        is_fresh_publish_ts(snap.video_pub_ts, now);
                    push_video = !cache.last_video_bvid.empty() && fresh_video;
                    cache.last_video_bvid = snap.video_bvid;
                    cache.last_video_title = snap.video_title;
                    if (snap.video_pub_ts > 0) {
                        cache.last_video_pub_ts = snap.video_pub_ts;
                    }
                    cache_dirty = true;
                }
            }

            if (snap.has_live && snap.live_status != cache.last_live_status) {
                push_live_on =
                    (cache.last_live_status != 1 && snap.live_status == 1);
                push_live_off = (cache.last_live_status == 1);
                cache.last_live_status = snap.live_status;
                cache.last_live_room_id = snap.live_room_id;
                cache.last_live_title = snap.live_title;
                cache_dirty = true;
            }
            else if (snap.has_live) {
                if (snap.live_room_id != 0 &&
                    snap.live_room_id != cache.last_live_room_id) {
                    cache.last_live_room_id = snap.live_room_id;
                    cache_dirty = true;
                }
                if (!snap.live_title.empty() &&
                    snap.live_title != cache.last_live_title) {
                    cache.last_live_title = snap.live_title;
                    cache_dirty = true;
                }
            }
        }

        if (!(push_dynamic || push_video || push_live_on || push_live_off)) {
            continue;
        }

        std::vector<std::string> update_types;
        if (push_dynamic) {
            update_types.push_back("feed");
        }
        if (push_video) {
            update_types.push_back("video");
        }
        if (push_live_on) {
            update_types.push_back("live_on");
        }
        if (push_live_off) {
            update_types.push_back("live_off");
        }
        p->setlog(LOG::INFO,
                  fmt::format("bili poll update uid={} name={} types={}", uid,
                              snap.name.empty() ? "<unknown>" : snap.name,
                              join_tokens(update_types)));

        const auto &groups = uid_to_groups[uid];
        for (groupid_t gid : groups) {
            const auto try_send = [&](const std::string &msg) {
                std::string err;
                (void)send_group_msg_checked(p, gid, msg, err);
            };

            if (push_dynamic) {
                try_send(build_dynamic_push_message(uid, snap));
            }
            if (push_video) {
                try_send(build_video_push_message(uid, snap));
            }
            if (push_live_on) {
                try_send(build_live_on_push_message(uid, snap));
            }
            if (push_live_off) {
                try_send(build_live_off_push_message(uid, snap));
            }
        }
    }

    if (cache_dirty) {
        std::lock_guard<std::mutex> lock(mutex_);
        save_cache_unlocked();
    }
}
