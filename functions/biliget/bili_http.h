#pragma once

#include <jsoncpp/json/json.h>
#include <string>

namespace biliget_http {

Json::Value parse_json_relaxed(const std::string &raw);
Json::Value safe_get_json(const std::string &host, const std::string &path);
std::string shorten_text(const std::string &raw, size_t max_len);
void set_cookie_override(const std::string &cookie);
void clear_cookie_override();
bool has_cookie_override();

struct debug_result_t {
    bool has_json = false;
    int code = -9999;
    std::string message;
    uint64_t uid = 0;
    uint64_t room_id = 0;
    int live_status = -1;
    std::string detail;
};

debug_result_t debug_endpoint(const std::string &host, const std::string &path);

} // namespace biliget_http
