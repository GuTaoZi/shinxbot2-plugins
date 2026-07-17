#pragma once

#include "processable.h"

#include <map>

class RP : public processable {
private:
    std::map<userid_t, std::pair<uint32_t, std::string>>
        reply_content; // user_id -> <possible, messsage>

    // Full on-demand usage (rp.help), gated by permission level: management
    // commands (add/del/list) only appear for group_admin/bot_admin.
    static std::string detailed_help(help_level_t level);

public:
    RP();
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    bool reload(const msg_meta &conf) override;
    // Brief one-liner only — this is what feeds the aggregated bot.help list.
    // Detailed usage lives in detailed_help(), reachable via rp.help.
    std::string help() override;
    void save();
};

DECLARE_FACTORY_FUNCTIONS_HEADER