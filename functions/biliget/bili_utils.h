#pragma once

#include "bot.h"
#include "utils.h"

#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

namespace biliget_utils {

bool str_all_digits(const std::string &s);
int64_t json_to_i64(const Json::Value &v, int64_t def = 0);
std::string first_non_empty(const std::initializer_list<std::string> &cands);
std::string url_encode(const std::string &s);

std::string compact_cover_url_169(const std::string &url);
std::string compact_cover_url_keep_ratio(const std::string &url);
std::string compact_cover_url_square(const std::string &url);
std::string compact_cover_url_avatar(const std::string &url);

std::string up_name_with_uid(userid_t uid, const std::string &name);
std::string live_jump_link(userid_t uid, uint64_t room_id);
std::string join_tokens(const std::vector<std::string> &tokens);

bool str_to_u64_if_digits(const std::string &s, uint64_t &out);
bool is_dynamic_id_newer_than(const std::string &curr, const std::string &prev);
bool is_fresh_publish_ts(int64_t ts, std::time_t now,
                         int64_t fresh_window_sec = 6 * 3600);

std::string api_fail_reason(const Json::Value &root,
                            const std::string &api_name);
std::string mask_cookie_text(const std::string &cookie);

bool send_group_msg_checked(bot *p, groupid_t gid, const std::string &message,
                            std::string &err);

} // namespace biliget_utils
