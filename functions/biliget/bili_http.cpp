#include "bili_http.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::mutex g_cookie_mu;
std::string g_cookie_override;

const std::map<std::string, std::string> kBiliHeaders = {
    {"user-agent",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
     "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
    {"accept", "application/json,text/plain,*/*"},
    {"Referer", "https://www.bilibili.com/"},
    {"Connection", "keep-alive"},
};

const std::map<std::string, std::string> kBiliHeadersBare = {
    {"User-Agent",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
     "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
    {"Accept", "application/json,text/plain,*/*"},
    {"Connection", "keep-alive"},
};

const std::map<std::string, std::string> kLiveHeaders = {
    {"User-Agent",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
     "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
    {"Accept", "application/json,text/plain,*/*"},
    {"Referer", "https://live.bilibili.com/"},
    {"Connection", "keep-alive"},
};

std::vector<std::map<std::string, std::string>>
choose_headers(const std::string &host)
{
    if (host.find("api.live.bilibili.com") != std::string::npos) {
        return {kLiveHeaders, {}, kBiliHeadersBare};
    }
    if (host.find("api.bilibili.com") != std::string::npos) {
        return {kBiliHeaders, kBiliHeadersBare};
    }
    return {{}, kBiliHeadersBare};
}

const char *get_env_any(const char *k1, const char *k2)
{
    const char *v = std::getenv(k1);
    return v ? v : std::getenv(k2);
}

std::string get_cookie_override_copy()
{
    std::lock_guard<std::mutex> lock(g_cookie_mu);
    return g_cookie_override;
}

struct http_fetch_t {
    bool performed = false;
    int status = -1;
    std::string body;
    std::string err;
};

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    if (userdata == nullptr) {
        return 0;
    }
    std::string *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string pick_proxy_env(const std::string &host)
{
    const bool is_https = host.rfind("https://", 0) == 0;
    const char *v = is_https ? get_env_any("https_proxy", "HTTPS_PROXY")
                             : get_env_any("http_proxy", "HTTP_PROXY");
    if (!v) {
        v = get_env_any("http_proxy", "HTTP_PROXY");
    }
    return v ? std::string(v) : std::string();
}

http_fetch_t curl_get_raw(const std::string &host, const std::string &path,
                          const std::map<std::string, std::string> &headers,
                          bool use_proxy)
{
    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    http_fetch_t out;
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        out.err = "curl_easy_init failed";
        return out;
    }

    const std::string url = host + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);

    if (use_proxy) {
        const std::string proxy = pick_proxy_env(host);
        if (proxy.empty()) {
            out.err = "proxy requested but env is empty";
            curl_easy_cleanup(curl);
            return out;
        }
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    }
    else {
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    }

    curl_slist *hlist = nullptr;
    for (const auto &kv : headers) {
        const std::string one = kv.first + ": " + kv.second;
        hlist = curl_slist_append(hlist, one.c_str());
    }

    // Optional authenticated mode for web endpoints.
    if (host.find("api.bilibili.com") != std::string::npos) {
        std::string cookie = get_cookie_override_copy();
        if (cookie.empty()) {
            const char *env_cookie =
                get_env_any("BILIGET_COOKIE", "biliget_cookie");
            if (env_cookie && env_cookie[0] != '\0') {
                cookie = env_cookie;
            }
        }
        if (!cookie.empty()) {
            const std::string c = "Cookie: " + cookie;
            hlist = curl_slist_append(hlist, c.c_str());
        }
    }

    if (hlist != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    }

    const CURLcode rc = curl_easy_perform(curl);
    out.performed = (rc == CURLE_OK);
    if (rc != CURLE_OK) {
        out.err = curl_easy_strerror(rc);
    }
    long code = -1;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    out.status = static_cast<int>(code);

    if (hlist != nullptr) {
        curl_slist_free_all(hlist);
    }
    curl_easy_cleanup(curl);

    return out;
}

bool parse_json_silent(const std::string &raw, Json::Value &out)
{
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errs;
    std::istringstream iss(raw);
    return Json::parseFromStream(builder, iss, &out, &errs);
}

bool looks_like_html(const std::string &raw)
{
    return raw.find("<!DOCTYPE html") != std::string::npos ||
           raw.find("<html") != std::string::npos ||
           raw.find("<noscript") != std::string::npos;
}

} // namespace

namespace biliget_http {

void set_cookie_override(const std::string &cookie)
{
    std::lock_guard<std::mutex> lock(g_cookie_mu);
    g_cookie_override = cookie;
}

void clear_cookie_override()
{
    std::lock_guard<std::mutex> lock(g_cookie_mu);
    g_cookie_override.clear();
}

bool has_cookie_override()
{
    std::lock_guard<std::mutex> lock(g_cookie_mu);
    return !g_cookie_override.empty();
}

Json::Value parse_json_relaxed(const std::string &raw)
{
    Json::Value j;
    if (parse_json_silent(raw, j) && j.isObject()) {
        return j;
    }

    const size_t l = raw.find('{');
    const size_t r = raw.rfind('}');
    if (l != std::string::npos && r != std::string::npos && l < r) {
        Json::Value sub;
        if (parse_json_silent(raw.substr(l, r - l + 1), sub) &&
            sub.isObject()) {
            return sub;
        }
    }
    return Json::Value();
}

Json::Value safe_get_json(const std::string &host, const std::string &path)
{
    const auto tries = choose_headers(host);
    Json::Value last_json;

    for (const auto &headers : tries) {
        for (bool use_proxy : {false, true}) {
            const http_fetch_t res =
                curl_get_raw(host, path, headers, use_proxy);
            if (res.status == 412) {
                continue;
            }
            if (!res.body.empty()) {
                if (looks_like_html(res.body)) {
                    continue;
                }
                Json::Value j = parse_json_relaxed(res.body);
                if (j.isObject()) {
                    last_json = j;
                    if (!j.isMember("code") || j["code"].asInt() == 0) {
                        return j;
                    }
                }
            }
        }
    }
    return last_json;
}

std::string shorten_text(const std::string &raw, size_t max_len)
{
    if (raw.size() <= max_len) {
        return raw;
    }
    return raw.substr(0, max_len) + "...";
}

debug_result_t debug_endpoint(const std::string &host, const std::string &path)
{
    debug_result_t out;
    const auto tries = choose_headers(host);

    std::string last_detail;
    for (size_t hi = 0; hi < tries.size(); ++hi) {
        const auto &headers = tries[hi];
        for (bool use_proxy : {false, true}) {
            const http_fetch_t res =
                curl_get_raw(host, path, headers, use_proxy);

            if (!res.body.empty()) {
                Json::Value j = parse_json_relaxed(res.body);
                if (j.isObject()) {
                    out.has_json = true;
                    out.code = j.get("code", -9999).asInt();
                    out.message = j.get("message", j.get("msg", "")).asString();

                    const Json::Value data = j["data"];
                    if (data.isObject()) {
                        out.uid = data.get("uid", 0).asUInt64();
                        out.room_id = data.get("room_id", 0).asUInt64();
                        out.live_status = data.get("live_status", -1).asInt();

                        // get_status_info_by_uids often returns a uid-keyed
                        // object map.
                        if (out.uid == 0 && out.room_id == 0) {
                            for (const auto &k : data.getMemberNames()) {
                                const Json::Value one = data[k];
                                if (!one.isObject()) {
                                    continue;
                                }
                                out.uid = one.get("uid", 0).asUInt64();
                                out.room_id = one.get("room_id", 0).asUInt64();
                                out.live_status =
                                    one.get("live_status", -1).asInt();
                                if (out.uid == 0 && !k.empty()) {
                                    if (std::all_of(k.begin(), k.end(),
                                                    [](unsigned char ch) {
                                                        return std::isdigit(
                                                                   ch) != 0;
                                                    })) {
                                        out.uid = std::strtoull(k.c_str(),
                                                                nullptr, 10);
                                    }
                                }
                                break;
                            }
                        }
                    }

                    std::ostringstream oss;
                    oss << "header_try=" << hi
                        << " proxy=" << (use_proxy ? 1 : 0) << " mode=curl"
                        << " status=" << res.status;
                    if (!res.err.empty()) {
                        oss << " err='" << shorten_text(res.err, 80) << "'";
                    }
                    out.detail = oss.str();
                    return out;
                }

                std::ostringstream oss;
                oss << "header_try=" << hi << " proxy=" << (use_proxy ? 1 : 0)
                    << " mode=curl" << " status=" << res.status << " raw='"
                    << shorten_text(res.body, 120) << "'";
                last_detail = oss.str();
                continue;
            }

            std::ostringstream oss;
            oss << "header_try=" << hi << " proxy=" << (use_proxy ? 1 : 0)
                << " mode=curl" << " status=" << res.status << " ex='"
                << shorten_text(res.err.empty() ? "empty response" : res.err,
                                120)
                << "'";
            last_detail = oss.str();
        }
    }

    out.detail = last_detail;
    return out;
}

} // namespace biliget_http
