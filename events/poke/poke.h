#pragma once

#include "eventprocess.h"

#include <chrono>
#include <mutex>
#include <set>
#include <string>

class poke : public eventprocess {
private:
    std::set<userid_t> no_poke_users;
    std::set<userid_t> no_poke_groups;
    std::chrono::steady_clock::time_point lastExecutionTime_{};
    std::mutex mutex_;
    std::chrono::duration<int64_t> minInterval;
    // One hitokoto per user per (bot-local, i.e. Shanghai) day, persisted so a
    // restart or hot-swap doesn't hand out a second one.
    std::string hitokoto_date_;
    std::set<userid_t> hitokoto_users_today_;

    void load_hitokoto_state();
    void save_hitokoto_state() const;
    bool take_daily_hitokoto(userid_t user_id);

public:
    poke();
    void process(bot *p, Json::Value J);
    bool check(bot *p, Json::Value J);
};

DECLARE_FACTORY_FUNCTIONS_HEADER