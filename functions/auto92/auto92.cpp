#include "auto92.h"

#include "utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr const char *CMD_HELP = "*92.help";
constexpr const char *CMD_REBUILD = "*92.rebuild";
constexpr const char *CMD_CLEAR_CACHE = "*92.clear_cache";
constexpr const char *CMD_PRECOMPUTE_PREFIX = "*92.precompute ";
constexpr const char *CMD_PREFIX = "*92 ";

constexpr const char *SPECIAL_92 = "92";
constexpr const char *SPECIAL_929 = "929";
constexpr const char *SPECIAL_92929 = "92929";

constexpr int64_t SEARCH_TARGET_ABS_LIMIT = 2'000'000;
constexpr int SEARCH_MAX_TERMS = 8;
constexpr int SEARCH_TIME_BUDGET_MS = 250;

using expr_map_t = std::unordered_map<int64_t, std::string>;

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

// --- parenthesis beautify: fully-paren tree -> minimal parens by priority ---
struct expr_node {
    bool leaf = true;
    std::string num;
    char op = 0;
    std::unique_ptr<expr_node> l, r;
};

std::unique_ptr<expr_node> parse_paren_expr(const std::string &s, size_t &p)
{
    if (p >= s.size()) {
        return nullptr;
    }
    if (std::isdigit((unsigned char)s[p])) {
        auto n = std::make_unique<expr_node>();
        const size_t st = p;
        while (p < s.size() && std::isdigit((unsigned char)s[p])) {
            ++p;
        }
        n->num = s.substr(st, p - st);
        return n;
    }
    if (s[p] != '(') {
        return nullptr;
    }
    ++p;
    auto l = parse_paren_expr(s, p);
    if (!l || p >= s.size()) {
        return nullptr;
    }
    const char op = s[p++];
    auto r = parse_paren_expr(s, p);
    if (!r || p >= s.size() || s[p] != ')') {
        return nullptr;
    }
    ++p;
    auto n = std::make_unique<expr_node>();
    n->leaf = false;
    n->op = op;
    n->l = std::move(l);
    n->r = std::move(r);
    return n;
}

int op_prec(char op)
{
    if (op == '+' || op == '-') {
        return 1;
    }
    if (op == '*' || op == '/') {
        return 2;
    }
    if (op == '^') {
        return 3;
    }
    return 0;
}

std::string serialize_min_paren(const expr_node *n)
{
    if (n->leaf) {
        return n->num;
    }
    const int p = op_prec(n->op);
    const bool right_assoc = (n->op == '^');
    std::string ls = serialize_min_paren(n->l.get());
    std::string rs = serialize_min_paren(n->r.get());
    if (!n->l->leaf) {
        const int lp = op_prec(n->l->op);
        if (lp < p || (lp == p && right_assoc)) {
            ls = "(" + ls + ")";
        }
    }
    if (!n->r->leaf) {
        const int rp = op_prec(n->r->op);
        if (rp < p || (rp == p && !right_assoc)) {
            rs = "(" + rs + ")";
        }
    }
    return ls + n->op + rs;
}

// Drop parentheses made redundant by operator priority/associativity. Value-
// preserving (same-precedence right children keep their parens, so integer
// -/÷ evaluation is unchanged). Falls back to raw if not fully-parenthesized.
std::string beautify_expr(const std::string &expr)
{
    size_t p = 0;
    auto root = parse_paren_expr(expr, p);
    if (!root || p != expr.size()) {
        return simplify_display_expr(expr);
    }
    return serialize_min_paren(root.get());
}
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

void auto92::ensure_horner_digits() const
{
    if (horner_ready_) {
        return;
    }
    struct dtok {
        int64_t value;
        std::string text;
        char last;
        bool has2;
    };
    std::vector<dtok> t9, t2;
    for (int d = 1; d <= 5; ++d) {
        for (char f : {'9', '2'}) {
            std::string s = make_alternating_token(d, f);
            int64_t val;
            if (!parse_i64(s, val)) {
                continue;
            }
            (f == '9' ? t9 : t2)
                .push_back({val, s, s.back(), s.find('2') != std::string::npos});
        }
    }
    const auto by_len = [](const dtok &a, const dtok &b) {
        if (a.text.size() != b.text.size()) {
            return a.text.size() < b.text.size();
        }
        return a.value < b.value;
    };
    std::sort(t9.begin(), t9.end(), by_len);
    std::sort(t2.begin(), t2.end(), by_len);

    const auto sub_idx = [](char last, bool has2) {
        return (last == '2' ? 2 : 0) + (has2 ? 1 : 0);
    };

    for (int target = 0; target <= 91; ++target) {
        std::unordered_map<int64_t, std::array<int, 4>> failed;
        // A base-92 "digit": 9-start, 2-end alternating block equal to target.
        std::function<bool(int64_t, char, bool, int, std::string &,
                           const std::string &)>
            dfs = [&](int64_t cur, char last, bool has2, int rem,
                      std::string &out, const std::string &expr) -> bool {
            if (cur == target && has2 && last == '2') {
                out = expr;
                return true;
            }
            if (rem <= 0) {
                return false;
            }
            const int idx = sub_idx(last, has2);
            auto it = failed.find(cur);
            if (it != failed.end() && it->second[idx] >= rem) {
                return false;
            }
            const char need = (last == '9') ? '2' : '9';
            const std::vector<dtok> &toks = (need == '9') ? t9 : t2;
            for (const dtok &t : toks) {
                for (char op : {'+', '-', '*', '^', '/'}) {
                    int64_t nv = 0;
                    bool ok = false;
                    switch (op) {
                    case '+':
                        ok = checked_add(cur, t.value, nv);
                        break;
                    case '-':
                        ok = checked_sub(cur, t.value, nv);
                        break;
                    case '*':
                        ok = checked_mul(cur, t.value, nv);
                        break;
                    case '^':
                        ok = checked_pow_int(cur, t.value, nv);
                        break;
                    case '/':
                        ok = (t.value != 0 && cur % t.value == 0);
                        if (ok) {
                            nv = cur / t.value;
                        }
                        break;
                    default:
                        break;
                    }
                    if (!ok || nv < -2'000'000 || nv > 2'000'000) {
                        continue;
                    }
                    if (dfs(nv, t.last, has2 || t.has2, rem - 1, out,
                            "(" + expr + op + t.text + ")")) {
                        return true;
                    }
                }
            }
            failed[cur][idx] = std::max(failed[cur][idx], rem);
            return false;
        };
        std::string result;
        for (int depth = 1; depth <= 7 && result.empty(); ++depth) {
            failed.clear();
            for (const dtok &t1 : t9) {
                std::string out;
                if (dfs(t1.value, t1.last, t1.has2, depth - 1, out, t1.text)) {
                    result = out;
                    break;
                }
            }
        }
        horner_digits_[static_cast<size_t>(target)] = result;
    }
    horner_ready_ = true;
}

// A 9-start/2-end block equal to v in base 92 (Horner). Zero digits are skipped
// (no "+0"), and exact even-length alternating tokens are used directly.
std::string auto92::build_horner_block(uint64_t v) const
{
    ensure_horner_digits();
    if (v <= 91) {
        return horner_digits_[static_cast<size_t>(v)];
    }
    if (v <= (uint64_t)LLONG_MAX) {
        auto it = exact_token_.find((int64_t)v);
        if (it != exact_token_.end() && !it->second.empty() &&
            it->second.back() == '2') {
            return it->second;
        }
    }
    const uint64_t q = v / 92;
    const uint64_t r = v % 92;
    const std::string inner = "(" + build_horner_block(q) + "*92)";
    if (r == 0) {
        return inner; // skip the redundant "+0" (was "+(92-92)")
    }
    return "(" + inner + "+" + horner_digits_[static_cast<size_t>(r)] + ")";
}

// Express any value: prefer a clean power (t^9 / t^2) or an exact alternating
// token, otherwise the base-92 block builder. Each base-92 digit is itself a
// 9-start/2-end block, so the whole chain stays strictly 9,2,9,2-alternating.
std::string auto92::build_horner(uint64_t v) const
{
    ensure_horner_digits();
    // Clean power t^e, with e (2 or 9) chaining after t's last digit.
    for (const auto &kv : exact_token_) {
        if (kv.first < 2 || kv.second.empty()) {
            continue;
        }
        const int e = (kv.second.back() == '2') ? 9 : 2;
        unsigned __int128 p = 1;
        bool overflow = false;
        for (int i = 0; i < e; ++i) {
            p *= (unsigned __int128)(uint64_t)kv.first;
            if (p > (unsigned __int128)UINT64_MAX) {
                overflow = true;
                break;
            }
        }
        if (!overflow && (uint64_t)p == v) {
            return kv.second + "^" + (e == 9 ? "9" : "2");
        }
    }
    // Exact alternating token (e.g. 929, 929292929).
    if (v <= (uint64_t)LLONG_MAX) {
        auto it = exact_token_.find((int64_t)v);
        if (it != exact_token_.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return build_horner_block(v);
}

std::string auto92::express_nonneg(int64_t v) const
{
    auto memo_it = memo_expr_.find(v);
    if (memo_it != memo_expr_.end()) {
        return memo_it->second;
    }

    const int64_t abs_v = v >= 0 ? v : -v;
    if (abs_v > SEARCH_TARGET_ABS_LIMIT) {
        // Too large for the tiered search; use the guaranteed base-92 builder.
        std::string big = (v >= 0) ? build_horner((uint64_t)v) : "";
        memo_expr_[v] = big;
        return big;
    }

    // Alternating 9/2 literals (short-first), split by leading digit.
    struct tok {
        int64_t value;
        std::string text;
        char last;
        bool has2;
    };
    std::vector<tok> tok9, tok2;
    for (int d = 1; d <= 7; ++d) {
        for (char f : {'9', '2'}) {
            std::string s = make_alternating_token(d, f);
            int64_t val;
            if (!parse_i64(s, val)) {
                continue;
            }
            (f == '9' ? tok9 : tok2)
                .push_back({val, s, s.back(), s.find('2') != std::string::npos});
        }
    }
    const auto by_len = [](const tok &a, const tok &b) {
        if (a.text.size() != b.text.size()) {
            return a.text.size() < b.text.size();
        }
        return a.value < b.value;
    };
    std::sort(tok9.begin(), tok9.end(), by_len);
    std::sort(tok2.begin(), tok2.end(), by_len);

    const int64_t value_limit = std::min<int64_t>(
        50'000'000, std::max<int64_t>(2'000'000, abs_v * 3 + 100'000));
    // Operator order: prefer clear + then - * ^, division last.
    static const char *const OPS = "+-*^/";

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(SEARCH_TIME_BUDGET_MS);
    bool timed_out = false;
    uint64_t steps = 0;
    // value -> max remaining-depth already explored-and-failed, per (last, has2).
    std::unordered_map<int64_t, std::array<int, 4>> failed;
    const auto sub_idx = [](char last, bool has2) {
        return (last == '2' ? 2 : 0) + (has2 ? 1 : 0);
    };

    // Iterative-deepening DFS: the first solution found uses the fewest terms,
    // giving the shortest/clearest expression deterministically.
    std::function<bool(int64_t, char, bool, int, std::string &,
                       const std::string &)>
        dfs = [&](int64_t cur, char last, bool has2, int rem, std::string &out,
                  const std::string &expr) -> bool {
        if (cur == v && has2) {
            out = expr;
            return true;
        }
        if (rem <= 0) {
            return false;
        }
        if ((++steps & 1023u) == 0 &&
            std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            return false;
        }
        const int idx = sub_idx(last, has2);
        auto fit = failed.find(cur);
        if (fit != failed.end() && fit->second[idx] >= rem) {
            return false;
        }
        const char need = (last == '9') ? '2' : '9';
        const std::vector<tok> &toks = (need == '9') ? tok9 : tok2;
        for (const tok &t : toks) {
            for (const char *op = OPS; *op != '\0'; ++op) {
                int64_t nv = 0;
                bool ok = false;
                switch (*op) {
                case '+':
                    ok = checked_add(cur, t.value, nv);
                    break;
                case '-':
                    ok = checked_sub(cur, t.value, nv);
                    break;
                case '*':
                    ok = checked_mul(cur, t.value, nv);
                    break;
                case '^':
                    ok = checked_pow_int(cur, t.value, nv);
                    break;
                case '/':
                    ok = (t.value != 0 && cur % t.value == 0);
                    if (ok) {
                        nv = cur / t.value;
                    }
                    break;
                default:
                    break;
                }
                if (!ok || nv < -value_limit || nv > value_limit) {
                    continue;
                }
                if (dfs(nv, t.last, has2 || t.has2, rem - 1, out,
                        "(" + expr + *op + t.text + ")")) {
                    return true;
                }
                if (timed_out) {
                    return false;
                }
            }
        }
        failed[cur][idx] = std::max(failed[cur][idx], rem);
        return false;
    };

    std::string result;
    for (int depth = 1; depth <= SEARCH_MAX_TERMS && !timed_out; ++depth) {
        for (const tok &t1 : tok9) {
            std::string out;
            if (dfs(t1.value, t1.last, t1.has2, depth - 1, out, t1.text)) {
                result = out;
                break;
            }
            if (timed_out) {
                break;
            }
        }
        if (!result.empty()) {
            break;
        }
    }

    // Insurance: keep only a result that truly evaluates to v and is legal.
    if (!result.empty()) {
        int64_t check_v;
        if (!eval_fully_paren_expr(result, check_v) || check_v != v ||
            !is_global_alt_92_expr(result)) {
            result.clear();
        }
    }

    // The IDDFS may run out of budget/terms; the base-92 builder always finds a
    // (longer) legal expression, so every non-negative value is answerable.
    if (result.empty() && v >= 0) {
        result = build_horner((uint64_t)v);
    }

    memo_expr_[v] = result;
    return result;
}

std::string auto92::express_u64(uint64_t v, int depth) const
{
    (void)depth;
    auto pit = persisted_expr_.find(v);
    if (pit != persisted_expr_.end() && is_global_alt_92_expr(pit->second)) {
        return pit->second;
    }

    // For values within the tiered range, express_nonneg gives the clean/short
    // form (and itself falls back to the base-92 builder if the search misses).
    if (v <= (uint64_t)LLONG_MAX) {
        std::string expr = express_nonneg((int64_t)v);
        if (!expr.empty() && is_global_alt_92_expr(expr)) {
            return expr;
        }
    }

    // Anything larger (up to uint64) is answered by the guaranteed builder.
    return build_horner(v);
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

        std::string canonical; // fully-parenthesized form (persisted/cached)
        std::string display;    // beautified form shown to the user
        if (is_negative) {
            int64_t n;
            if (!parse_i64(raw, n)) {
                conf.p->cq_send("这个负数太极端了（LLONG_MIN），请换一个。",
                                conf);
                return true;
            }
            const std::string inner = express_nonneg(-n);
            if (!inner.empty()) {
                canonical = "-(" + inner + ")";
                display = "-(" + beautify_expr(inner) + ")";
            }
        }
        else {
            canonical = express_u64(u64);
            if (!canonical.empty()) {
                display = beautify_expr(canonical);
            }
        }

        if (canonical.empty()) {
            conf.p->cq_send(
                "未在资源边界内找到表达式（已限制时间/状态以保护服务器）。",
                conf);
            return true;
        }

        if (!is_negative && u64 <= 2000000) {
            save_persisted_entry(u64, canonical);
        }

        conf.p->cq_send(raw + " = " + display, conf);
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
           cmd_match_prefix(m, {CMD_PRECOMPUTE_PREFIX, CMD_PREFIX});
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
