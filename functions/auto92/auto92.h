#pragma once

#include "processable.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class auto92 : public processable {
private:
    struct entry {
        int64_t value;
        std::string token;
    };

    std::vector<entry> cache_;
    std::unordered_map<int64_t, std::string> exact_token_;
    mutable std::unordered_map<int64_t, std::string> memo_expr_;
    mutable std::unordered_map<uint64_t, std::string> persisted_expr_;
    std::string cache_path_;
    std::string persisted_path_;

    static constexpr int kMaxInputDigits = 19;
    static constexpr int kMaxTokenDigits = 18;

    bool parse_i64(const std::string &s, int64_t &out) const;
    bool parse_u64(const std::string &s, uint64_t &out) const;
    std::string i64_to_string(int64_t v) const;

    std::string make_alternating_token(int digits, char first) const;
    void rebuild_cache_file();
    void load_or_build_cache();
    void load_persisted_cache();
    void save_persisted_entry(uint64_t value, const std::string &expr) const;

    std::string express_nonneg(int64_t v) const;
    std::string express_u64(uint64_t v, int depth = 0) const;

public:
    auto92();

    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;

    std::string help() override;
    std::string help(const msg_meta &conf,
                     help_level_t level = help_level_t::public_only) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
