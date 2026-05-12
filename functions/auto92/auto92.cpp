#include "auto92.h"

#include "utils.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr const char *CMD_HELP = "*92.help";
constexpr const char *CMD_REBUILD = "*92.rebuild";
constexpr const char *CMD_CLEAR_CACHE = "*92.clear_cache";
constexpr const char *CMD_PRECOMPUTE_PREFIX = "*92.precompute ";
constexpr const char *CMD_PREFIX = "*92 ";
constexpr const char *CMD_PREFIX_COMPAT = "92 ";

constexpr const char *SPECIAL_92 = "92";
constexpr const char *SPECIAL_929 = "929";
constexpr const char *SPECIAL_92929 = "92929";

constexpr int64_t SEARCH_TARGET_ABS_LIMIT = 2'000'000;
constexpr int SEARCH_MAX_TERMS = 8;
constexpr size_t SEARCH_STATE_BUDGET = 120000;
constexpr size_t SEARCH_BEAM_WIDTH = 3500;
constexpr int SEARCH_TIME_BUDGET_MS = 120;

using expr_map_t = std::unordered_map<int64_t, std::string>;

struct alt_literal {
    int64_t value;
    std::string text;
    char first;
    char last;
    bool has2;
};

bool starts_with_9_and_has_2(const std::string &expr)
{
    return !expr.empty() && expr.front() == '9' &&
           expr.find('2') != std::string::npos;
}

bool is_global_alt_92_expr(const std::string &expr)
{
    char prev = 0;
    bool seen_digit = false;
    bool has2 = false;

    for (char c : expr) {
        if (c != '9' && c != '2') {
            continue;
        }
        if (!seen_digit) {
            if (c != '9') {
                return false;
            }
            seen_digit = true;
        }
        else if (c == prev) {
            return false;
        }
        if (c == '2') {
            has2 = true;
        }
        prev = c;
    }
    return seen_digit && has2;
}

bool checked_add(int64_t a, int64_t b, int64_t &out)
{
    __int128 t = (__int128)a + (__int128)b;
    if (t < LLONG_MIN || t > LLONG_MAX) {
        return false;
    }
    out = (int64_t)t;
    return true;
}

bool checked_sub(int64_t a, int64_t b, int64_t &out)
{
    __int128 t = (__int128)a - (__int128)b;
    if (t < LLONG_MIN || t > LLONG_MAX) {
        return false;
    }
    out = (int64_t)t;
    return true;
}

bool checked_mul(int64_t a, int64_t b, int64_t &out)
{
    __int128 t = (__int128)a * (__int128)b;
    if (t < LLONG_MIN || t > LLONG_MAX) {
        return false;
    }
    out = (int64_t)t;
    return true;
}

bool checked_pow_int(int64_t base, int64_t exp, int64_t &out)
{
    if (exp < 0 || exp > 10) {
        return false;
    }
    int64_t acc = 1;
    for (int64_t i = 0; i < exp; ++i) {
        int64_t t;
        if (!checked_mul(acc, base, t)) {
            return false;
        }
        acc = t;
    }
    out = acc;
    return true;
}

bool eval_fully_paren_expr_impl(const std::string &expr, size_t &pos,
                                int64_t &out)
{
    if (pos >= expr.size()) {
        return false;
    }

    if (std::isdigit((unsigned char)expr[pos])) {
        int64_t v = 0;
        while (pos < expr.size() && std::isdigit((unsigned char)expr[pos])) {
            int d = expr[pos] - '0';
            int64_t nv;
            if (!checked_mul(v, 10, nv) || !checked_add(nv, d, v)) {
                return false;
            }
            ++pos;
        }
        out = v;
        return true;
    }

    if (expr[pos] != '(') {
        return false;
    }
    ++pos;

    int64_t lhs;
    if (!eval_fully_paren_expr_impl(expr, pos, lhs)) {
        return false;
    }
    if (pos >= expr.size()) {
        return false;
    }
    const char op = expr[pos++];

    int64_t rhs;
    if (!eval_fully_paren_expr_impl(expr, pos, rhs)) {
        return false;
    }
    if (pos >= expr.size() || expr[pos] != ')') {
        return false;
    }
    ++pos;

    switch (op) {
    case '+':
        return checked_add(lhs, rhs, out);
    case '-':
        return checked_sub(lhs, rhs, out);
    case '*':
        return checked_mul(lhs, rhs, out);
    case '/':
        if (rhs == 0 || lhs % rhs != 0) {
            return false;
        }
        out = lhs / rhs;
        return true;
    case '^':
        return checked_pow_int(lhs, rhs, out);
    default:
        return false;
    }
}

bool eval_fully_paren_expr(const std::string &expr, int64_t &out)
{
    size_t pos = 0;
    if (!eval_fully_paren_expr_impl(expr, pos, out)) {
        return false;
    }
    return pos == expr.size();
}

bool is_alt_token(const std::string &s)
{
    if (s.empty()) {
        return false;
    }
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] == s[i - 1]) {
            return false;
        }
    }
    return true;
}

bool pick_first_last_digit(const std::string &expr, char &first, char &last)
{
    bool seen = false;
    first = 0;
    last = 0;
    for (char c : expr) {
        if (c != '9' && c != '2') {
            continue;
        }
        if (!seen) {
            first = c;
            seen = true;
        }
        last = c;
    }
    return seen;
}

bool wrapped_by_outer_parens(const std::string &s)
{
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') {
        return false;
    }
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') {
            ++depth;
        }
        else if (s[i] == ')') {
            --depth;
            if (depth == 0 && i + 1 < s.size()) {
                return false;
            }
        }
        if (depth < 0) {
            return false;
        }
    }
    return depth == 0;
}

std::string simplify_display_expr(std::string expr)
{
    while (wrapped_by_outer_parens(expr)) {
        expr = expr.substr(1, expr.size() - 2);
    }
    return expr;
}

std::string trim_sign_space(const std::string &s) { return trim(s); }
} // namespace

auto92::auto92()
{
    cache_path_ = bot_resource_path(nullptr, "auto92/alt92_cache.txt");
    persisted_path_ = bot_resource_path(nullptr, "auto92/expr_cache.txt");
    load_or_build_cache();
    load_persisted_cache();
}

bool auto92::parse_i64(const std::string &s, int64_t &out) const
{
    std::string t = trim_sign_space(s);
    if (t.empty()) {
        return false;
    }

    size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = (t[0] == '-');
        i = 1;
    }

    if (i >= t.size()) {
        return false;
    }

    __int128 val = 0;
    for (; i < t.size(); ++i) {
        char c = t[i];
        if (c < '0' || c > '9') {
            return false;
        }
        val *= 10;
        val += (c - '0');
        if (!neg && val > LLONG_MAX) {
            return false;
        }
        if (neg && val > (__int128)LLONG_MAX + 1) {
            return false;
        }
    }

    if (neg) {
        if (val == (__int128)LLONG_MAX + 1) {
            out = LLONG_MIN;
        }
        else {
            out = -(int64_t)val;
        }
    }
    else {
        out = (int64_t)val;
    }
    return true;
}

bool auto92::parse_u64(const std::string &s, uint64_t &out) const
{
    std::string t = trim_sign_space(s);
    if (t.empty()) {
        return false;
    }
    if (t[0] == '-') {
        return false;
    }
    if (t[0] == '+') {
        t = t.substr(1);
    }
    if (t.empty()) {
        return false;
    }

    __int128 val = 0;
    for (char c : t) {
        if (c < '0' || c > '9') {
            return false;
        }
        val = val * 10 + (c - '0');
        if (val > (__int128)UINT64_MAX) {
            return false;
        }
    }
    out = (uint64_t)val;
    return true;
}

std::string auto92::i64_to_string(int64_t v) const { return std::to_string(v); }

std::string auto92::make_alternating_token(int digits, char first) const
{
    std::string token;
    token.reserve((size_t)digits);
    for (int i = 0; i < digits; ++i) {
        if (i % 2 == 0) {
            token.push_back(first);
        }
        else {
            token.push_back(first == '9' ? '2' : '9');
        }
    }
    return token;
}

void auto92::rebuild_cache_file()
{
    fs::path p(cache_path_);
    fs::create_directories(p.parent_path());

    std::ofstream out(cache_path_, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        set_global_log(LOG::ERROR,
                       "auto92: failed to write cache file: " + cache_path_);
        return;
    }

    // Descending order to make greedy lookup straightforward.
    for (int d = kMaxTokenDigits; d >= 1; --d) {
        for (char first : {'9', '2'}) {
            std::string tok = make_alternating_token(d, first);
            int64_t val;
            if (!parse_i64(tok, val)) {
                continue;
            }
            out << i64_to_string(val) << ' ' << tok << '\n';
        }
    }
}

void auto92::load_or_build_cache()
{
    cache_.clear();
    exact_token_.clear();
    memo_expr_.clear();

    std::ifstream in(cache_path_);
    if (!in.is_open()) {
        rebuild_cache_file();
        in.open(cache_path_);
    }

    if (!in.is_open()) {
        set_global_log(LOG::ERROR,
                       "auto92: missing cache file and auto-generate failed: " +
                           cache_path_);
    }
    else {
        std::string value_str;
        std::string token;
        while (in >> value_str >> token) {
            int64_t v;
            if (!parse_i64(value_str, v) || token.empty()) {
                continue;
            }
            cache_.push_back({v, token});
        }
    }

    bool legacy_cache = true;
    for (const auto &it : cache_) {
        if (!it.token.empty() && it.token.front() == '2') {
            legacy_cache = false;
            break;
        }
    }

    if (!cache_.empty() && legacy_cache) {
        // Old cache with only one alternation group, rebuild to include both.
        rebuild_cache_file();
        cache_.clear();
        exact_token_.clear();

        std::ifstream in2(cache_path_);
        std::string value_str;
        std::string token;
        while (in2 >> value_str >> token) {
            int64_t v;
            if (!parse_i64(value_str, v) || token.empty()) {
                continue;
            }
            cache_.push_back({v, token});
        }
    }

    if (cache_.empty()) {
        // Fallback: build in memory so the module is still usable.
        for (int d = kMaxTokenDigits; d >= 1; --d) {
            for (char first : {'9', '2'}) {
                std::string tok = make_alternating_token(d, first);
                int64_t v;
                if (parse_i64(tok, v)) {
                    cache_.push_back({v, tok});
                }
            }
        }
    }

    // Keep one shortest 9-leading literal per exact value for fast exact hits.
    for (const auto &it : cache_) {
        if (!starts_with_9_and_has_2(it.token)) {
            continue;
        }
        auto pos = exact_token_.find(it.value);
        if (pos == exact_token_.end() || it.token.size() < pos->second.size() ||
            (it.token.size() == pos->second.size() && it.token < pos->second)) {
            exact_token_[it.value] = it.token;
        }
    }

    std::sort(cache_.begin(), cache_.end(),
              [](const entry &a, const entry &b) { return a.value > b.value; });

    cache_.erase(std::unique(cache_.begin(), cache_.end(),
                             [](const entry &a, const entry &b) {
                                 return a.value == b.value &&
                                        a.token == b.token;
                             }),
                 cache_.end());
}

void auto92::load_persisted_cache()
{
    persisted_expr_.clear();
    std::ifstream in(persisted_path_);
    if (!in.is_open()) {
        return;
    }
    std::string k;
    std::string expr;
    while (in >> k) {
        std::getline(in, expr);
        expr = trim(expr);
        if (expr.empty()) {
            continue;
        }
        uint64_t v;
        if (!parse_u64(k, v)) {
            continue;
        }
        persisted_expr_[v] = expr;
    }
}

void auto92::save_persisted_entry(uint64_t value, const std::string &expr) const
{
    if (expr.empty()) {
        return;
    }
    auto it = persisted_expr_.find(value);
    if (it != persisted_expr_.end() && it->second == expr) {
        return;
    }
    persisted_expr_[value] = expr;

    fs::path p(persisted_path_);
    fs::create_directories(p.parent_path());
    std::ofstream out(persisted_path_, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    for (const auto &kv : persisted_expr_) {
        out << kv.first << " " << kv.second << "\n";
    }
}

std::string auto92::express_nonneg(int64_t v) const
{
    auto memo_it = memo_expr_.find(v);
    if (memo_it != memo_expr_.end()) {
        return memo_it->second;
    }

    const int64_t abs_v = v >= 0 ? v : -v;
    if (abs_v > SEARCH_TARGET_ABS_LIMIT) {
        memo_expr_[v] = "";
        return "";
    }
    const int64_t value_limit = std::min<int64_t>(6'000'000, abs_v * 6 + 3000);

    struct lit {
        int64_t value;
        std::string text;
        char first;
        char last;
        bool has2;
    };

    std::vector<lit> literals;
    literals.reserve(cache_.size());
    std::unordered_set<std::string> seen;
    for (const auto &e : cache_) {
        if (e.value <= 0 || e.token.empty()) {
            continue;
        }
        if (!is_alt_token(e.token)) {
            continue;
        }
        if ((int)e.token.size() > 6) {
            continue;
        }
        if (!seen.insert(e.token).second) {
            continue;
        }
        literals.push_back({e.value, e.token, e.token.front(), e.token.back(),
                            e.token.find('2') != std::string::npos});
    }

    // Reuse previously solved expressions as composite literals.
    for (const auto &kv : persisted_expr_) {
        if (kv.first == 0 || kv.first > (uint64_t)value_limit) {
            continue;
        }
        const std::string &expr = kv.second;
        if (!is_global_alt_92_expr(expr)) {
            continue;
        }
        char first = 0;
        char last = 0;
        if (!pick_first_last_digit(expr, first, last)) {
            continue;
        }
        if (!seen.insert(expr).second) {
            continue;
        }
        literals.push_back({(int64_t)kv.first, expr, first, last,
                            expr.find('2') != std::string::npos});
    }

    std::sort(literals.begin(), literals.end(), [](const lit &a, const lit &b) {
        if (a.text.size() != b.text.size()) {
            return a.text.size() < b.text.size();
        }
        return a.text < b.text;
    });

    std::shuffle(literals.begin(), literals.end(), get_engine());

    struct state {
        int64_t value;
        char last_digit;
        bool has2;
        int terms;
        std::string expr;
    };

    const int max_terms = SEARCH_MAX_TERMS;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(SEARCH_TIME_BUDGET_MS);

    auto make_key = [](int64_t value, char last, bool has2, int terms) {
        return std::to_string(value) + "|" + std::string(1, last) + "|" +
               (has2 ? "1" : "0") + "|" + std::to_string(terms);
    };

    std::unordered_map<std::string, size_t> best_len;
    std::vector<state> frontier;
    frontier.reserve(256);

    for (const auto &l : literals) {
        if (l.first != '9') {
            continue;
        }
        state s{l.value, l.last, l.has2, 1, l.text};
        best_len[make_key(s.value, s.last_digit, s.has2, s.terms)] =
            s.expr.size();
        frontier.push_back(std::move(s));
    }

    auto accept = [&](const state &s) -> bool {
        if (s.value != v || !s.has2 || !is_global_alt_92_expr(s.expr)) {
            return false;
        }
        int64_t check_v;
        if (!eval_fully_paren_expr(s.expr, check_v) || check_v != v) {
            return false;
        }
        memo_expr_[v] = s.expr;
        return true;
    };

    for (const auto &s : frontier) {
        if (accept(s)) {
            return s.expr;
        }
    }

    for (int depth = 1; depth < max_terms; ++depth) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::vector<state> next;
        next.reserve(frontier.size() * 6);

        for (const auto &cur : frontier) {
            const char need_first = (cur.last_digit == '9') ? '2' : '9';
            for (const auto &l : literals) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                if (l.first != need_first) {
                    continue;
                }
                const bool nhas2 = cur.has2 || l.has2;

                auto push = [&](char op, int64_t nv) {
                    if (nv < -value_limit || nv > value_limit) {
                        return;
                    }
                    state ns{nv, l.last, nhas2, cur.terms + 1,
                             "(" + cur.expr + op + l.text + ")"};

                    const std::string key =
                        make_key(ns.value, ns.last_digit, ns.has2, ns.terms);
                    auto it = best_len.find(key);
                    if (it != best_len.end() && it->second <= ns.expr.size()) {
                        return;
                    }
                    best_len[key] = ns.expr.size();
                    if (best_len.size() > SEARCH_STATE_BUDGET) {
                        return;
                    }

                    if (accept(ns)) {
                        next.clear();
                        next.push_back(std::move(ns));
                        return;
                    }
                    next.push_back(std::move(ns));
                };

                int64_t t;
                if (checked_add(cur.value, l.value, t)) {
                    push('+', t);
                    if (!next.empty() && next.back().value == v &&
                        next.back().expr == memo_expr_[v]) {
                        return memo_expr_[v];
                    }
                }
                if (checked_sub(cur.value, l.value, t)) {
                    push('-', t);
                    if (!next.empty() && next.back().value == v &&
                        next.back().expr == memo_expr_[v]) {
                        return memo_expr_[v];
                    }
                }
                if (checked_mul(cur.value, l.value, t)) {
                    push('*', t);
                    if (!next.empty() && next.back().value == v &&
                        next.back().expr == memo_expr_[v]) {
                        return memo_expr_[v];
                    }
                }
                if (l.value != 0 && cur.value % l.value == 0) {
                    push('/', cur.value / l.value);
                    if (!next.empty() && next.back().value == v &&
                        next.back().expr == memo_expr_[v]) {
                        return memo_expr_[v];
                    }
                }
                if (checked_pow_int(cur.value, l.value, t)) {
                    push('^', t);
                    if (!next.empty() && next.back().value == v &&
                        next.back().expr == memo_expr_[v]) {
                        return memo_expr_[v];
                    }
                }
            }
        }

        if (next.size() > SEARCH_BEAM_WIDTH) {
            auto score = [v](const state &s) {
                __int128 d = (__int128)s.value - (__int128)v;
                if (d < 0) {
                    d = -d;
                }
                return std::pair<__int128, size_t>(d, s.expr.size());
            };
            std::nth_element(next.begin(), next.begin() + SEARCH_BEAM_WIDTH,
                             next.end(), [&](const state &a, const state &b) {
                                 return score(a) < score(b);
                             });
            next.resize(SEARCH_BEAM_WIDTH);
        }

        frontier.swap(next);
        if (frontier.empty()) {
            break;
        }
    }

    memo_expr_[v] = "";
    return "";
}

std::string auto92::express_u64(uint64_t v, int depth) const
{
    auto pit = persisted_expr_.find(v);
    if (pit != persisted_expr_.end() && is_global_alt_92_expr(pit->second)) {
        return pit->second;
    }

    if (v <= (uint64_t)LLONG_MAX) {
        std::string expr = express_nonneg((int64_t)v);
        if (!expr.empty() && is_global_alt_92_expr(expr)) {
            return expr;
        }
    }

    if (depth > 20) {
        return "";
    }

    std::vector<std::pair<uint64_t, std::string>> tokens;
    tokens.reserve(exact_token_.size());
    for (const auto &kv : exact_token_) {
        if (kv.first <= 1) {
            continue;
        }
        uint64_t tv = (uint64_t)kv.first;
        if (tv > v) {
            continue;
        }
        tokens.push_back({tv, kv.second});
    }
    if (tokens.empty()) {
        return "";
    }

    std::shuffle(tokens.begin(), tokens.end(), get_engine());
    std::sort(tokens.begin(), tokens.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    size_t tries = 0;
    for (const auto &tk : tokens) {
        if (++tries > 48) {
            break;
        }
        const uint64_t base = tk.first;
        const std::string &tok = tk.second;
        const uint64_t q = v / base;
        const uint64_t r = v % base;
        if (q == 0) {
            continue;
        }

        if (q == 1) {
            if (r == 0) {
                if (is_global_alt_92_expr(tok)) {
                    return tok;
                }
                continue;
            }
            std::string re = express_u64(r, depth + 1);
            if (re.empty()) {
                continue;
            }
            std::string cand = "(" + tok + "+" + re + ")";
            if (is_global_alt_92_expr(cand)) {
                return cand;
            }
            continue;
        }

        std::string qe = express_u64(q, depth + 1);
        if (qe.empty()) {
            continue;
        }

        if (r == 0) {
            std::string cand = "(" + qe + "*" + tok + ")";
            if (is_global_alt_92_expr(cand)) {
                return cand;
            }
            continue;
        }

        std::string re = express_u64(r, depth + 1);
        if (re.empty()) {
            continue;
        }
        std::string cand = "((" + qe + "*" + tok + ")+" + re + ")";
        if (is_global_alt_92_expr(cand)) {
            return cand;
        }
    }

    return "";
}

void auto92::process(std::string message, const msg_meta &conf)
{
    std::string m = trim(message);
    const bool is_op_user = conf.p->is_op(conf.user_id);

    const auto handle_help = [&]() {
        conf.p->cq_send(help(conf, is_op_user ? help_level_t::bot_admin
                                              : help_level_t::public_only),
                        conf);
        return true;
    };

    const auto handle_rebuild = [&]() {
        rebuild_cache_file();
        load_or_build_cache();
        conf.p->cq_send("92缓存已重建", conf);
        return true;
    };

    const auto handle_clear_cache = [&]() {
        std::error_code ec;
        fs::remove(cache_path_, ec);
        fs::remove(persisted_path_, ec);
        load_or_build_cache();
        load_persisted_cache();
        conf.p->cq_send("92缓存已清空", conf);
        return true;
    };

    const auto handle_precompute = [&](const std::string &body) {
        int limit = 1000;
        std::string t = trim(body);
        if (!t.empty()) {
            int64_t v = my_string2int64(t);
            if (v > 0) {
                limit = (int)std::min<int64_t>(10000, v);
            }
        }

        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(1800);
        int solved = 0;
        int tested = 0;
        for (int i = 0; i <= limit; ++i) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            ++tested;
            if (persisted_expr_.find((uint64_t)i) != persisted_expr_.end()) {
                continue;
            }
            std::string expr = express_u64((uint64_t)i);
            if (!expr.empty()) {
                save_persisted_entry((uint64_t)i, expr);
                ++solved;
            }
        }

        auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        conf.p->cq_send("precompute done: tested=" + std::to_string(tested) +
                            ", solved=" + std::to_string(solved) +
                            ", cost=" + std::to_string(cost) + "ms",
                        conf);
        return true;
    };

    auto handle_eval = [&](const std::string &body) {
        std::string raw = trim(body);
        if (raw.empty()) {
            conf.p->cq_send("格式: *92 <数字>", conf);
            return true;
        }

        if (raw == SPECIAL_92) {
            conf.p->cq_send("咱是92的说！", conf);
            return true;
        }
        if (raw == SPECIAL_929) {
            conf.p->cq_send("929！", conf);
            return true;
        }
        if (raw == SPECIAL_92929) {
            conf.p->cq_send("92929~", conf);
            return true;
        }

        std::string abs_digits = raw;
        if (!abs_digits.empty() &&
            (abs_digits[0] == '+' || abs_digits[0] == '-')) {
            abs_digits = abs_digits.substr(1);
        }
        if ((int)abs_digits.size() > 20) {
            conf.p->cq_send("数字太大了，当前支持 uint64（最多 20 位）。",
                            conf);
            return true;
        }

        uint64_t u64 = 0;
        bool is_negative = !raw.empty() && raw[0] == '-';
        if (!is_negative && !parse_u64(raw, u64)) {
            conf.p->cq_send("数字过大或格式错误（当前支持 uint64 范围）。",
                            conf);
            return true;
        }

        std::string expr;
        if (is_negative) {
            int64_t n;
            if (!parse_i64(raw, n)) {
                conf.p->cq_send("这个负数太极端了（LLONG_MIN），请换一个。",
                                conf);
                return true;
            }
            expr = "-(" + express_nonneg(-n) + ")";
        }
        else {
            expr = express_u64(u64);
        }

        if (expr.empty()) {
            conf.p->cq_send(
                "未在资源边界内找到表达式（已限制时间/状态以保护服务器）。",
                conf);
            return true;
        }

        if (!is_negative && u64 <= 2000000) {
            save_persisted_entry(u64, expr);
        }

        conf.p->cq_send(raw + " = " + simplify_display_expr(expr), conf);
        return true;
    };

    const cmd_middleware_t op_only = [&]() { return is_op_user; };

    const std::vector<cmd_exact_rule> exact_rules = {
        {CMD_HELP, handle_help},
        {CMD_REBUILD, handle_rebuild, {op_only}},
        {CMD_CLEAR_CACHE, handle_clear_cache, {op_only}},
    };

    const std::vector<cmd_prefix_rule> prefix_rules = {
        {CMD_PRECOMPUTE_PREFIX,
         [&]() {
             std::string body;
             if (!cmd_strip_prefix(m, CMD_PRECOMPUTE_PREFIX, body)) {
                 return false;
             }
             return handle_precompute(body);
         },
         {op_only}},
        {CMD_PREFIX,
         [&]() {
             std::string body;
             if (!cmd_strip_prefix(m, CMD_PREFIX, body)) {
                 return false;
             }
             return handle_eval(body);
         }},
        {CMD_PREFIX_COMPAT,
         [&]() {
             std::string body;
             if (!cmd_strip_prefix(m, CMD_PREFIX_COMPAT, body)) {
                 return false;
             }
             return handle_eval(body);
         }},
    };

    bool handled = false;
    (void)cmd_try_dispatch(m, exact_rules, prefix_rules, handled);
    if (!handled) {
        conf.p->cq_send("格式: *92 <数字>，帮助: *92.help", conf);
    }
}

bool auto92::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    std::string m = trim(message);
    return cmd_match_exact(m, {CMD_HELP, CMD_REBUILD, CMD_CLEAR_CACHE}) ||
           cmd_match_prefix(
               m, {CMD_PRECOMPUTE_PREFIX, CMD_PREFIX, CMD_PREFIX_COMPAT});
}

std::string auto92::help()
{
    return "92论证器：*92 <数字>（支持 uint64，严格9/2交替表达）";
}

std::string auto92::help(const msg_meta &conf, help_level_t level)
{
    (void)conf;
    if (level == help_level_t::bot_admin) {
        return "92论证器\n"
               "*92 <数字>\n"
               "*92.help\n"
               "*92.rebuild (OP) 重建token缓存\n"
               "*92.clear_cache (OP) 清空所有缓存\n"
               "*92.precompute 1000|10000 (OP) 预热表达式缓存";
    }
    return "92论证器\n*92 <数字>\n*92.help";
}

DECLARE_FACTORY_FUNCTIONS(auto92)
