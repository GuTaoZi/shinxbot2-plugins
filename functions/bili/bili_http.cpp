#include "bili_http.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <openssl/evp.h>
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

// Reusable per-thread CURL handle. curl_easy_reset() clears options but KEEPS
// the handle's live connections, TLS session ids and DNS cache — so reusing one
// handle per worker thread gives HTTP keep-alive + TLS session reuse across
// requests (no fresh handshake per call). The handle is freed on thread exit.
struct tls_curl_handle {
    CURL *h = nullptr;
    tls_curl_handle() { h = curl_easy_init(); }
    ~tls_curl_handle()
    {
        if (h != nullptr) {
            curl_easy_cleanup(h);
        }
    }
    tls_curl_handle(const tls_curl_handle &) = delete;
    tls_curl_handle &operator=(const tls_curl_handle &) = delete;
};

http_fetch_t curl_get_raw(const std::string &host, const std::string &path,
                          const std::map<std::string, std::string> &headers,
                          bool use_proxy)
{
    static std::once_flag curl_once;
    std::call_once(curl_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    http_fetch_t out;
    thread_local tls_curl_handle tls_handle;
    CURL *curl = tls_handle.h;
    if (curl == nullptr) {
        out.err = "curl_easy_init failed";
        return out;
    }
    // Reset options between calls but keep the connection/session/DNS caches.
    curl_easy_reset(curl);

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
            return out; // handle is thread_local/reused; do not clean it up
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
                get_env_any("BILI_COOKIE", "bili_cookie");
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
    // NOTE: do not curl_easy_cleanup(curl) — the handle is thread_local and
    // reused so its keep-alive connections/TLS sessions survive to the next call.

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

namespace bili_http {

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

// ---- WBI signing (w_rid/wts) --------------------------------------------
namespace {

std::string md5_hex(const std::string &in)
{
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    std::string hex;
    if (ctx != nullptr && EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, in.data(), in.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, out, &len) == 1) {
        static const char *hx = "0123456789abcdef";
        hex.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) {
            hex.push_back(hx[out[i] >> 4]);
            hex.push_back(hx[out[i] & 0x0F]);
        }
    }
    if (ctx != nullptr) {
        EVP_MD_CTX_free(ctx);
    }
    return hex;
}

std::string url_encode_wbi(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        }
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// filename of a URL without directory or extension (img_url/sub_url -> key)
std::string url_key(const std::string &url)
{
    size_t slash = url.find_last_of('/');
    std::string name = (slash == std::string::npos) ? url : url.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

// bilibili's fixed mixin-key reorder table
const int kMixinTab[] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};

// Fetch (and cache ~6h) the WBI mixin key derived from /x/web-interface/nav.
std::string get_mixin_key()
{
    static std::mutex mu;
    static std::string cached;
    static std::time_t fetched_at = 0;

    std::lock_guard<std::mutex> lock(mu);
    const std::time_t now = std::time(nullptr);
    if (!cached.empty() && now - fetched_at < 6 * 3600) {
        return cached;
    }

    const http_fetch_t res =
        curl_get_raw("https://api.bilibili.com", "/x/web-interface/nav",
                     kBiliHeadersBare, false);
    if (!res.body.empty()) {
        Json::Value j = parse_json_relaxed(res.body);
        const Json::Value wbi = j["data"]["wbi_img"];
        const std::string img = url_key(wbi.get("img_url", "").asString());
        const std::string sub = url_key(wbi.get("sub_url", "").asString());
        if (!img.empty() && !sub.empty()) {
            const std::string orig = img + sub;
            std::string mixin;
            mixin.reserve(32);
            for (int idx : kMixinTab) {
                if (idx < static_cast<int>(orig.size())) {
                    mixin.push_back(orig[idx]);
                }
            }
            cached = mixin.substr(0, 32);
            fetched_at = now;
        }
    }
    return cached;
}

} // namespace

std::string wbi_sign_query(std::map<std::string, std::string> params)
{
    // Build sorted, url-encoded query (std::map iterates keys ascending).
    const auto build_query = [](const std::map<std::string, std::string> &p) {
        std::string q;
        for (const auto &kv : p) {
            if (!q.empty()) {
                q.push_back('&');
            }
            q += url_encode_wbi(kv.first) + "=" + url_encode_wbi(kv.second);
        }
        return q;
    };

    const std::string mixin = get_mixin_key();
    params["wts"] = std::to_string(std::time(nullptr));
    const std::string base = build_query(params);
    if (mixin.empty()) {
        return base; // best-effort: unsigned (will likely 412, but not worse)
    }
    const std::string w_rid = md5_hex(base + mixin);
    return base + "&w_rid=" + w_rid;
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

} // namespace bili_http
