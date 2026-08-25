#include "dateimg.h"

#include "dateimg_util.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

// ===================== subscriptions =====================

void dateimg::load_subs_unlocked() {
    std::map<groupid_t, std::string> subs;
    std::string last_fired;
    try {
        Json::Value root = string_to_json(readfile(subs_path_, "{}"));
        if (root.isObject()) {
            last_fired = root.get("last_fired", "").asString();
            const Json::Value &groups = root["groups"];
            if (groups.isObject()) {
                for (const auto &k : groups.getMemberNames()) {
                    try {
                        subs[static_cast<groupid_t>(std::stoull(k))] =
                            groups[k].asString();
                    } catch (...) {
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        set_global_log(LOG::WARNING,
                       std::string("dateimg: bad dateimg.json, keeping old: ") +
                           e.what());
        return;
    }
    subs_.swap(subs);
    last_fired_ymd_ = last_fired;
}

void dateimg::save_subs_unlocked() const {
    Json::Value root(Json::objectValue);
    Json::Value groups(Json::objectValue);
    for (const auto &kv : subs_) {
        groups[std::to_string(kv.first)] = kv.second;
    }
    root["groups"] = groups;
    root["last_fired"] = last_fired_ymd_;
    writefile(subs_path_, root.toStyledString(), false);
}

// ===================== daily blessing messages =====================

void dateimg::load_messages_unlocked() {
    bool enabled = true;
    std::string prefix = "猫好~，今天是";
    std::string lead;
    std::vector<std::string> sentences;
    try {
        Json::Value root = string_to_json(readfile(msgs_path_, "{}"));
        if (root.isObject()) {
            enabled = root.get("enabled", true).asBool();
            prefix = root.get("prefix", "猫好~，今天是").asString();
            lead = root.get("lead", "").asString();
            const Json::Value &arr = root["sentences"];
            if (arr.isArray()) {
                for (const auto &v : arr) {
                    if (v.isString()) {
                        sentences.push_back(v.asString());
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        set_global_log(
            LOG::WARNING,
            std::string("dateimg: bad messages.json, keeping old: ") +
                e.what());
        return;
    }
    msg_enabled_ = enabled;
    msg_prefix_ = prefix;
    msg_lead_ = lead;
    sentences_.swap(sentences);
}

// ===================== background config =====================

void dateimg::load_bgcfg_unlocked() {
    std::map<std::string, placement> placements;
    std::string default_font;
    std::string mode = "gif";
    bool lunar_on = true;
    double lunar_scale = 0.6;
    int output_max_px = 1280;
    try {
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (root.isObject()) {
            default_font = root.get("default_font", "").asString();
            output_max_px = root.get("output_max_px", 1280).asInt();
            lunar_on = root.get("lunar", true).asBool();
            lunar_scale = root.get("lunar_scale", 0.6).asDouble();
            mode = root.get("mode", "gif").asString();
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (mode != "png") {
                mode = "gif";
            }
            const Json::Value &bgs = root["backgrounds"];
            if (bgs.isObject()) {
                for (const auto &name : bgs.getMemberNames()) {
                    const Json::Value &j = bgs[name];
                    if (!j.isObject()) {
                        continue;
                    }
                    placement p;
                    p.x = j.get("x", 0).asDouble();
                    p.y = j.get("y", 0).asDouble();
                    p.font_size = j.get("font_size", 48).asDouble();
                    p.rotation = j.get("rotation", 0).asDouble();
                    p.color = j.get("color", "black").asString();
                    p.font = j.get("font", "").asString();
                    p.layout = j.get("layout", "single").asString();
                    if (p.layout != "double") {
                        p.layout = "single";
                    }
                    placements[name] = p;
                }
            }
        }
    } catch (const std::exception &e) {
        set_global_log(
            LOG::WARNING,
            std::string("dateimg: bad backgrounds.json, keeping old config: ") +
                e.what());
        return;
    }
    placements_.swap(placements);
    default_font_ = default_font;
    mode_ = mode;
    lunar_enabled_ = lunar_on;
    lunar_scale_ = lunar_scale;
    output_max_px_ = output_max_px;
}

// ===================== background scan / resolve =====================

std::vector<std::string> dateimg::available_backgrounds() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(bg_dir_, ec)) {
        return out;
    }
    try {
        for (const auto &e : fs::directory_iterator(bg_dir_, ec)) {
            if (e.is_regular_file() &&
                dutil::has_image_ext(e.path().filename().string())) {
                out.push_back(e.path().filename().string());
            }
        }
    } catch (const std::exception &e) {
        set_global_log(LOG::WARNING,
                       std::string("dateimg: scan backgrounds failed: ") +
                           e.what());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string dateimg::pick_background_unlocked(const std::string &pinned) const {
    std::vector<std::string> files = available_backgrounds();
    std::vector<std::string> candidates;
    for (const auto &f : files) {
        if (placements_.count(f)) {
            candidates.push_back(f);
        }
    }
    if (candidates.empty()) {
        return "";
    }
    if (!pinned.empty()) {
        for (const auto &c : candidates) {
            if (c == pinned) {
                return pinned;
            }
        }
    }
    return candidates[get_random(static_cast<int>(candidates.size()))];
}

std::string dateimg::resolve_font_unlocked(const placement &pl) const {
    const std::string name = pl.font.empty() ? default_font_ : pl.font;
    if (!name.empty()) {
        std::string p = (fs::path(font_dir_) / name).string();
        if (fs::exists(p)) {
            return p;
        }
    }
    std::error_code ec;
    if (fs::is_directory(font_dir_, ec)) {
        try {
            for (const auto &e : fs::directory_iterator(font_dir_, ec)) {
                if (e.is_regular_file()) {
                    std::string ext = e.path().extension().string();
                    std::transform(
                        ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return std::tolower(c); });
                    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
                        return e.path().string();
                    }
                }
            }
        } catch (const std::exception &e) {
            set_global_log(LOG::WARNING,
                           std::string("dateimg: scan fonts failed: ") +
                               e.what());
        }
    }
    return "";
}

void dateimg::invalidate_cache_unlocked(const std::string &bg_key) {
    const std::string stem =
        dutil::safe_stem(fs::path(bg_key).stem().string()) + "_";
    std::error_code ec;
    if (!fs::is_directory(out_dir_, ec)) {
        return;
    }
    try {
        for (const auto &e : fs::directory_iterator(out_dir_, ec)) {
            if (e.is_regular_file() &&
                e.path().filename().string().rfind(stem, 0) == 0) {
                fs::remove(e.path(), ec);
            }
        }
    } catch (...) {
    }
}

std::string dateimg::resolve_bg(const std::string &arg) const {
    const std::string a = trim(arg);
    if (a.empty()) {
        return "";
    }
    std::vector<std::string> files = available_backgrounds();
    // numeric index into the sorted list
    if (a.find_first_not_of("0123456789") == std::string::npos) {
        try {
            size_t idx = std::stoul(a);
            if (idx < files.size()) {
                return files[idx];
            }
        } catch (...) {
        }
        return "";
    }
    for (const auto &f : files) { // exact filename
        if (f == a) {
            return f;
        }
    }
    for (const auto &f : files) { // exact stem
        if (fs::path(f).stem().string() == a) {
            return f;
        }
    }
    for (const auto &f : files) { // sanitized-stem match
        if (dutil::safe_stem(fs::path(f).stem().string()) ==
            dutil::safe_stem(a)) {
            return f;
        }
    }
    return "";
}
