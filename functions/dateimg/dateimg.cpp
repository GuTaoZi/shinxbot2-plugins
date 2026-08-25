#include "dateimg.h"

#include "dateimg_util.h"
#include "lunar.h"
#include "render.h"
#include "utils.h"

#include "../../lib/help_utils.h"

#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <set>
#include <sstream>

namespace {
// Thread-safe local time (std::localtime shares a static tm).
std::tm local_tm(std::time_t t) {
    std::tm r{};
    localtime_r(&t, &r);
    return r;
}
} // namespace

dateimg::dateimg() {
    subs_path_ = bot_config_path(nullptr, "features/dateimg/dateimg.json");
    bgcfg_path_ = bot_config_path(nullptr, "features/dateimg/backgrounds.json");
    msgs_path_ = bot_config_path(nullptr, "features/dateimg/messages.json");
    bdays_path_ = bot_config_path(nullptr, "features/dateimg/birthdays.json");
    bg_dir_ = bot_resource_path(nullptr, "dateimg/backgrounds");
    font_dir_ = bot_resource_path(nullptr, "dateimg/fonts");
    out_dir_ = bot_resource_path(nullptr, "dateimg/out");
    std::error_code ec;
    fs::create_directories(out_dir_, ec);

    std::lock_guard<std::mutex> lock(mu_);
    load_subs_unlocked();
    load_bgcfg_unlocked();
    load_messages_unlocked();
    load_birthdays_unlocked();
}

// Compose the daily message: "<prefix>\n[image]\n<lead><random sentence>".
// Reads messages.json fresh; picks a fresh random sentence each call (so each
// group gets its own). Returns just the image if the feature is disabled.
std::string dateimg::compose_daily_message(const std::string &img_abs,
                                           groupid_t group_id) {
    const std::string img = "[CQ:image,file=file://" + img_abs + ",id=40000]";
    bool en = true;
    std::string prefix, lead, sentence;
    std::vector<std::string> wishes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_messages_unlocked();
        en = msg_enabled_;
        prefix = msg_prefix_;
        lead = msg_lead_;
        if (en && !sentences_.empty()) {
            sentence =
                sentences_[get_random(static_cast<int>(sentences_.size()))];
        }
        if (group_id != 0) {
            load_birthdays_unlocked();
            std::tm lt = local_tm(std::time(nullptr));
            wishes = birthday_wishes_unlocked(group_id, lt);
        }
    }
    std::string out;
    if (en && !prefix.empty()) {
        out += dutil::cq_escape_text(prefix) + "\n";
    }
    out += img;
    if (en && !sentence.empty()) {
        out += "\n" + dutil::cq_escape_text(lead) +
               dutil::cq_escape_text(sentence);
    }
    for (const auto &w : wishes) { // birthday wishes below the message
        out += "\n" + dutil::cq_escape_text(w);
    }
    return out;
}

std::string dateimg::today_date_text() {
    std::tm lt = local_tm(std::time(nullptr));
    return fmt::format("{}月{}日", lt.tm_mon + 1, lt.tm_mday);
}

// ===================== render glue =====================

std::string dateimg::render_image(const std::string &bg_name, bool force) {
    std::tm lt = local_tm(std::time(nullptr));
    const int yr = lt.tm_year + 1900, mon = lt.tm_mon + 1, day = lt.tm_mday;
    const std::string date_tag = fmt::format("{:04}{:02}{:02}", yr, mon, day);
    const std::string date_text = fmt::format("{}月{}日", mon, day);

    placement pl;
    std::string font_abs, mode, out_path;
    bool two = false, lunar_on = true;
    double lunar_scale = 0.6;
    int omax = 1280;
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_bgcfg_unlocked(); // always render from freshest config on disk
        auto it = placements_.find(bg_name);
        if (it == placements_.end()) {
            set_global_log(LOG::WARNING,
                           "dateimg: no placement for background " + bg_name);
            return "";
        }
        pl = it->second;
        font_abs = resolve_font_unlocked(pl);
        mode = (mode_ == "png") ? "png" : "gif";
        two = (pl.layout == "double");
        lunar_on = lunar_enabled_;
        lunar_scale = lunar_scale_;
        omax = output_max_px_;
        const std::string stem =
            dutil::safe_stem(fs::path(bg_name).stem().string());
        out_path = (fs::path(out_dir_) / (stem + "_" + date_tag + "." + mode))
                       .string();
        if (force) {
            invalidate_cache_unlocked(bg_name);
        }
    }

    if (!force && fs::exists(out_path)) {
        return fs::absolute(out_path).string(); // reuse today's image
    }
    if (font_abs.empty()) {
        set_global_log(LOG::ERROR,
                       "dateimg: no usable font (put a .ttf/.otf in " +
                           font_dir_ + " or set default_font)");
        return "";
    }
    const std::string bg_path = (fs::path(bg_dir_) / bg_name).string();
    if (!fs::exists(bg_path)) {
        set_global_log(LOG::ERROR, "dateimg: background missing: " + bg_path);
        return "";
    }

    dateimg_render::RenderSpec s;
    s.bg_path = bg_path;
    s.font_path = font_abs;
    s.x = pl.x;
    s.y = pl.y;
    s.font_size = pl.font_size;
    s.rotation = pl.rotation;
    s.color = pl.color;
    s.two_line = two;
    if (two) {
        auto pr = dutil::split_month_day(date_text);
        s.main_line1 = pr.first;
        s.main_line2 = pr.second;
    } else {
        s.main_line1 = date_text;
    }
    s.lunar_text = lunar_on ? lunar::date_cn(yr, mon, day) : "";
    s.lunar_scale = lunar_scale;
    s.output_max_px = omax;
    s.mode = mode;
    s.out_path = out_path;

    std::string err;
    if (!dateimg_render::render(s, err)) {
        set_global_log(LOG::ERROR,
                       std::string("dateimg: render failed: ") + err);
        return "";
    }
    return fs::absolute(out_path).string();
}

void dateimg::send_forward_images(const std::vector<std::string> &paths,
                                  const msg_meta &conf) {
    if (paths.empty()) {
        return;
    }
    Json::Value messages(Json::arrayValue);
    const std::string uin = std::to_string(conf.p->get_botqq());
    for (const auto &abs : paths) {
        Json::Value node(Json::objectValue), data(Json::objectValue);
        node["type"] = "node";
        data["name"] = "日期图";
        data["uin"] = uin;
        data["content"] =
            string_to_messageArr("[CQ:image,file=file://" + abs + ",id=40000]");
        node["data"] = data;
        messages.append(node);
    }
    Json::Value req(Json::objectValue);
    std::string endpoint;
    if (conf.message_type == "group") {
        req["group_id"] = Json::UInt64(conf.group_id);
        endpoint = "send_group_forward_msg";
    } else {
        req["user_id"] = Json::UInt64(conf.user_id);
        endpoint = "send_private_forward_msg";
    }
    req["messages"] = messages;
    const std::string resp = conf.p->cq_send(endpoint, req);
    Json::Value r = string_to_json(resp);
    if (r.isObject() && r.get("status", "ok").asString() == "failed") {
        set_global_log(
            LOG::WARNING,
            "dateimg: forward send failed, falling back to inline: " + resp);
        for (const auto &abs : paths) { // fallback: send images one by one
            conf.p->cq_send("[CQ:image,file=file://" + abs + ",id=40000]",
                            conf);
        }
    }
}

// ===================== daily push (shared 500ms Timer) =====================

void dateimg::push_all(bot *p) {
    std::vector<std::pair<groupid_t, std::string>> targets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_bgcfg_unlocked();
        for (const auto &kv : subs_) {
            targets.emplace_back(kv.first, pick_background_unlocked(kv.second));
        }
    }
    for (const auto &t : targets) {
        if (t.second.empty()) {
            continue;
        }
        const std::string img = render_image(t.second, false);
        if (img.empty()) {
            continue;
        }
        msg_meta conf{"group", 0, t.first, 0, p};
        p->cq_send(compose_daily_message(img, t.first), conf);
    }
}

void dateimg::daily_task(bot *p) {
    std::tm lt = local_tm(std::time(nullptr));
    if (lt.tm_hour != 0 || lt.tm_min != 0) {
        return;
    }
    const std::string ymd = fmt::format("{:04}{:02}{:02}", lt.tm_year + 1900,
                                        lt.tm_mon + 1, lt.tm_mday);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (last_fired_ymd_ == ymd) {
            return;
        }
        last_fired_ymd_ = ymd;
        save_subs_unlocked();
    }
    push_all(p);
}

void dateimg::set_callback(std::function<void(std::function<void(bot *p)>)> f) {
    f([this](bot *p) { daily_task(p); });
}

// ===================== framework hooks =====================

bool dateimg::check(std::string message, const msg_meta &conf) {
    if (dutil::has_image(message)) { // pending upload follow-up
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_upload_.count(conf.user_id)) {
            return true;
        }
    }
    const std::string raw = trim(message);
    if (conf.message_type == "group" && starts_with(raw, "date.")) {
        return true; // birthday commands (merged from the old plugin)
    }
    std::string body;
    if (!cmd_strip_prefix(raw, dutil::CMD_PREFIX, body)) {
        return false;
    }
    std::istringstream iss(body);
    std::string sub;
    iss >> sub;
    static const std::set<std::string> known = {
        "",     "help",  "list",   "on",    "off",    "enable", "disable", "bg",
        "test", "addbg", "setcfg", "delbg", "rename", "mode",   "msg"};
    return known.count(sub) > 0;
}

void dateimg::process(std::string message, const msg_meta &conf) {
    const std::string raw = trim(message);

    // pending bot-admin upload: this message carries the image we waited for
    if (dutil::has_image(message) && !starts_with(raw, dutil::CMD_PREFIX)) {
        std::string pend;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = pending_upload_.find(conf.user_id);
            if (it != pending_upload_.end()) {
                pend = it->second;
                pending_upload_.erase(it);
            }
        }
        if (pend.empty()) {
            return;
        }
        if (conf.message_type == "group" || !conf.p->is_op(conf.user_id)) {
            return;
        }
        std::string url;
        if (dutil::parse_image_url(message, url)) {
            store_bg_from_url(url, pend, conf);
        } else {
            cq_send(conf.p, "没找到图片链接，请重试 dateimg.addbg。", conf);
        }
        return;
    }

    // birthday commands (merged from the old `birthday` plugin), group-only
    if (conf.message_type == "group" && starts_with(raw, "date.")) {
        cmd_date(raw, conf);
        return;
    }

    std::string body;
    if (!cmd_strip_prefix(raw, dutil::CMD_PREFIX, body)) {
        return;
    }
    std::istringstream iss(body);
    std::string sub;
    iss >> sub;
    std::string arg;
    std::getline(iss, arg);
    arg = trim(arg);

    const bool can_manage = conf.p->is_op(conf.user_id) ||
                            (conf.message_type == "group" &&
                             is_group_op(conf.p, conf.group_id, conf.user_id));

    if (sub == "help" || sub.empty()) {
        cq_send(conf.p, detailed_help(resolve_help_level(conf)), conf);
        return;
    }
    if (sub == "list") {
        cmd_list(conf);
        return;
    }

    // test + bot-admin config commands: private chat, bot-op only.
    if (sub == "test" || sub == "addbg" || sub == "setcfg" || sub == "delbg" ||
        sub == "rename" || sub == "mode" || sub == "msg") {
        if (conf.message_type == "group") {
            cq_send(conf.p, "该命令仅限私聊使用。", conf);
            return;
        }
        if (!conf.p->is_op(conf.user_id)) {
            cq_send(conf.p, "仅 bot 管理可使用该命令。", conf);
            return;
        }
        if (sub == "test") {
            cmd_test(arg, conf);
        } else if (sub == "addbg") {
            cmd_addbg(arg, message, conf);
        } else if (sub == "setcfg") {
            cmd_setcfg(arg, conf);
        } else if (sub == "delbg") {
            cmd_delbg(arg, conf);
        } else if (sub == "rename") {
            cmd_rename(arg, conf);
        } else if (sub == "mode") {
            cmd_mode(arg, conf);
        } else {
            cmd_msg(arg, conf);
        }
        return;
    }

    // enable/disable/bg: group-scoped, group-admin or bot-admin only.
    if (sub == "enable" || sub == "on" || sub == "disable" || sub == "off" ||
        sub == "bg") {
        if (conf.message_type != "group") {
            cq_send(conf.p, "请在群里使用该命令。", conf);
            return;
        }
        if (!can_manage) {
            cq_send(conf.p, "只有群管理或 bot 管理可以操作。", conf);
            return;
        }
        if (sub == "enable" || sub == "on") {
            cmd_on(conf);
        } else if (sub == "disable" || sub == "off") {
            cmd_off(conf);
        } else {
            cmd_bg(arg, conf);
        }
        return;
    }

    cq_send(conf.p, "未知命令。帮助: dateimg.help", conf);
}

bool dateimg::reload(const msg_meta &conf) {
    (void)conf;
    std::lock_guard<std::mutex> lock(mu_);
    load_subs_unlocked();
    load_bgcfg_unlocked();
    return true;
}

void dateimg::set_backup_files(archivist *p, const std::string &name) {
    p->add_path(name, bot_resource_path(nullptr, "dateimg/"),
                "resource/dateimg/");
    p->add_path(name + "_cfg", bot_config_path(nullptr, "features/dateimg/"),
                "config/features/dateimg/");
}

std::string dateimg::help() {
    return "每日日期图: 00:00 推送 x月x日 日期贴纸(含农历)。帮助: dateimg.help";
}

DECLARE_FACTORY_FUNCTIONS(dateimg)
