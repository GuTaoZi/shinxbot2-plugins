#pragma once

#include "shinxbot2-api/include/processable.h"
#include "shinxbot2-api/include/utils.h"

// Canonical permission-level resolution for a plugin's help(conf, level)
// override. Mirrors the framework's own bot.help aggregator exactly
// (src/bots/shinxbot_meta.cpp, handle_bot_help): bot_admin requires a
// PRIVATE chat from an op; group_admin requires a GROUP chat from a group
// admin/owner; everyone else is public_only.
//
// Several plugins used to re-derive this independently (and at least one
// diverged from the framework's rule, e.g. granting bot_admin to an op inside
// a group). Prefer this shared helper for new plugins so behavior stays
// consistent with the aggregated `bot.help` output.
inline help_level_t resolve_help_level(const msg_meta &conf)
{
    if (conf.p == nullptr) {
        return help_level_t::public_only;
    }
    if (conf.message_type == "private" && conf.p->is_op(conf.user_id)) {
        return help_level_t::bot_admin;
    }
    if (conf.message_type == "group" &&
        is_group_op(conf.p, conf.group_id, conf.user_id)) {
        return help_level_t::group_admin;
    }
    return help_level_t::public_only;
}
