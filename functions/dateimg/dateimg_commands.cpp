#include "dateimg.h"

#include "dateimg_util.h"
#include "utils.h"

#include "../../lib/help_utils.h"

#include <Magick++.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <set>
#include <sstream>

namespace {
std::atomic<uint64_t> g_dl_seq{0};
} // namespace

// ===================== group commands =====================

void dateimg::cmd_on(const msg_meta &conf) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!subs_.count(conf.group_id)) {
            subs_[conf.group_id] = ""; // "" = random each day
        }
        save_subs_unlocked();
    }
    cq_send(conf.p,
            "本群已启用每日 00:00 日期图(随机背景)。用 dateimg.bg <序号|名字> "
            "可固定背景。",
            conf);
}

void dateimg::cmd_off(const msg_meta &conf) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        subs_.erase(conf.group_id);
        save_subs_unlocked();
    }
    cq_send(conf.p, "本群已停用每日日期图。", conf);
}

void dateimg::cmd_bg(const std::string &arg, const msg_meta &conf) {
    const std::string name = trim(arg);
    if (name.empty()) {
        cq_send(conf.p, "用法: dateimg.bg <序号|名字|random>", conf);
        return;
    }
    std::string want;
    if (name == "random" || name == "随机") {
        want = "";
    } else {
        want = resolve_bg(name);
        if (want.empty()) {
            cq_send(conf.p, "没有这个背景（用 dateimg.list 查看序号/名字）。",
                    conf);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        subs_[conf.group_id] = want;
        save_subs_unlocked();
    }
    cq_send(conf.p,
            want.empty() ? "已设为每日随机背景。" : ("背景已设为: " + want),
            conf);
}

void dateimg::cmd_list(const msg_meta &conf) {
    std::string out;
    {
        std::ostringstream oss;
        std::lock_guard<std::mutex> lock(mu_);
        load_bgcfg_unlocked();
        std::vector<std::string> files = available_backgrounds();
        oss << "输出模式: " << mode_
            << "  农历: " << (lunar_enabled_ ? "开" : "关");
        oss << "\n背景 (" << files.size() << "):";
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string &f = files[i];
            oss << "\n  [" << i << "] " << f;
            auto it = placements_.find(f);
            if (it == placements_.end()) {
                oss << "  [缺少配置]";
            } else {
                const placement &p = it->second;
                oss << fmt::format(
                    "  {} x={:.0f} y={:.0f} 字号={:.0f} 旋转={:.0f}",
                    p.layout == "double" ? "双行" : "单行", p.x, p.y,
                    p.font_size, p.rotation);
            }
        }
        oss << "\n字体:";
        std::error_code ec;
        if (fs::is_directory(font_dir_, ec)) {
            try {
                for (const auto &e : fs::directory_iterator(font_dir_, ec)) {
                    if (!e.is_regular_file()) {
                        continue;
                    }
                    std::string ext = e.path().extension().string();
                    std::transform(
                        ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return std::tolower(c); });
                    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
                        oss << "\n  " << e.path().filename().string();
                    }
                }
            } catch (...) {
            }
        }
        auto it = subs_.find(conf.group_id);
        oss << "\n本群: "
            << (it == subs_.end()
                    ? "未启用"
                    : (it->second.empty() ? "已启用(随机)"
                                          : ("已启用(" + it->second + ")")));
        out = oss.str();
    }
    cq_send(conf.p, out, conf);
}

// ===================== bot-admin (private) commands =====================

void dateimg::cmd_test(const std::string &arg, const msg_meta &conf) {
    const std::string a = trim(arg);
    if (a.empty()) {
        // all configured backgrounds -> one folded forward message
        std::vector<std::string> files;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_bgcfg_unlocked();
            for (const auto &f : available_backgrounds()) {
                if (placements_.count(f)) {
                    files.push_back(f);
                }
            }
        }
        if (files.empty()) {
            cq_send(conf.p, "没有已配置的背景。用 dateimg.addbg 上传。", conf);
            return;
        }
        std::vector<std::string> imgs;
        for (const auto &f : files) {
            std::string img = render_image(f, true);
            if (!img.empty()) {
                imgs.push_back(img);
            }
        }
        if (imgs.empty()) {
            cq_send(conf.p, "全部生成失败，请检查日志。", conf);
            return;
        }
        send_forward_images(imgs, conf);
        return;
    }
    // single background
    const std::string bg = resolve_bg(a);
    if (bg.empty()) {
        cq_send(conf.p, "没有这个背景（用 dateimg.list 查看序号/名字）。",
                conf);
        return;
    }
    const std::string img = render_image(bg, true);
    if (img.empty()) {
        cq_send(conf.p, "生成失败（可能缺少位置配置/字体，见日志）。", conf);
        return;
    }
    // preview the FULL daily message (greeting + image + a random sentence;
    // birthday wishes only show in a group context)
    cq_send(conf.p, compose_daily_message(img, conf.group_id), conf);
}

void dateimg::send_preview(const std::string &bg_name, const msg_meta &conf) {
    std::string img = render_image(bg_name, true);
    if (!img.empty()) {
        cq_send(conf.p, "[CQ:image,file=file://" + img + ",id=40000]", conf);
    }
}

void dateimg::store_bg_from_url(const std::string &url, const std::string &key,
                                const msg_meta &conf) {
    // download (network I/O) outside the lock, then transcode to a real PNG
    // (the QQ url may serve JPEG/GIF regardless of our filename).
    const std::string tmp_name =
        "._dl_" + std::to_string(g_dl_seq.fetch_add(1)) + ".bin";
    const std::string bg_abs = (fs::path(bg_dir_) / key).string();
    size_t w = 0, h = 0;
    try {
        std::error_code ec;
        fs::create_directories(bg_dir_, ec);
        download(url, out_dir_, tmp_name);
        const std::string tmp = (fs::path(out_dir_) / tmp_name).string();
        Magick::Image im;
        im.read(tmp);
        w = im.columns();
        h = im.rows();
        im.magick("PNG");
        im.write(bg_abs);
        fs::remove(tmp, ec);
    } catch (const std::exception &e) {
        cq_send(conf.p, std::string("下载/转换失败: ") + e.what(), conf);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (!root.isObject()) {
            root = Json::Value(Json::objectValue);
        }
        Json::Value &bgs = root["backgrounds"];
        if (!bgs.isObject()) {
            bgs = Json::Value(Json::objectValue);
        }
        if (!bgs.isMember(key)) {
            Json::Value p(Json::objectValue);
            p["x"] = w ? w / 2.0 : 400.0;
            p["y"] = h ? h / 2.0 : 300.0;
            p["font_size"] = h ? std::max(24.0, h * 0.10) : 48.0;
            p["rotation"] = 0.0;
            p["color"] = "black";
            p["font"] = "";
            p["layout"] = "single";
            bgs[key] = p;
        }
        writefile(bgcfg_path_, root.toStyledString(), false);
        invalidate_cache_unlocked(key);
        load_bgcfg_unlocked();
    }
    cq_send(conf.p,
            fmt::format("已保存背景 {} ({}x{})，默认单行居中。方形/圆形牌子用 "
                        "dateimg.setcfg {} layout double 设为双行。",
                        key, w, h, key),
            conf);
    send_preview(key, conf);
}

void dateimg::cmd_addbg(const std::string &arg, const std::string &message,
                        const msg_meta &conf) {
    std::istringstream iss(arg);
    std::string name;
    iss >> name;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_upload_.erase(conf.user_id);
    }
    if (name.empty()) {
        cq_send(
            conf.p,
            "用法: dateimg.addbg <名字>  然后发送图片(或同一条消息附带图片)。",
            conf);
        return;
    }
    const std::string key = dutil::bg_key_from_name(name);
    std::string url;
    if (dutil::parse_image_url(message, url)) {
        store_bg_from_url(url, key, conf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_upload_[conf.user_id] = key;
    }
    cq_send(conf.p, "收到，请把要用作背景 [" + key + "] 的图片发给我。", conf);
}

void dateimg::cmd_setcfg(const std::string &arg, const msg_meta &conf) {
    std::istringstream iss(arg);
    std::string ref, field, value;
    iss >> ref >> field;
    std::getline(iss, value);
    value = trim(value);
    if (ref.empty() || field.empty() || value.empty()) {
        cq_send(conf.p,
                "用法: dateimg.setcfg <序号|名字> "
                "<x|y|font_size|rotation|color|font|layout> <值>",
                conf);
        return;
    }
    const std::string bg = resolve_bg(ref);
    if (bg.empty()) {
        cq_send(conf.p, "没有这个背景（用 dateimg.list 查看）。", conf);
        return;
    }
    static const std::set<std::string> numf = {"x", "y", "font_size",
                                               "rotation"};
    static const std::set<std::string> strf = {"color", "font", "layout"};
    if (!numf.count(field) && !strf.count(field)) {
        cq_send(conf.p,
                "未知字段。可用: x y font_size rotation color font layout",
                conf);
        return;
    }
    double dv = 0;
    if (numf.count(field)) {
        try {
            dv = std::stod(value);
        } catch (...) {
            cq_send(conf.p, "数值无效。", conf);
            return;
        }
    }
    if (field == "layout" && value != "single" && value != "double") {
        cq_send(conf.p, "layout 只能是 single 或 double。", conf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (!root.isObject()) {
            root = Json::Value(Json::objectValue);
        }
        Json::Value &bgs = root["backgrounds"];
        if (!bgs.isObject()) {
            bgs = Json::Value(Json::objectValue);
        }
        if (!bgs.isMember(bg)) { // configuring a freshly-dropped background
            Json::Value p(Json::objectValue);
            p["x"] = 0;
            p["y"] = 0;
            p["font_size"] = 48;
            p["rotation"] = 0;
            p["color"] = "black";
            p["font"] = "";
            p["layout"] = "single";
            bgs[bg] = p;
        }
        if (numf.count(field)) {
            bgs[bg][field] = dv;
        } else {
            bgs[bg][field] = value;
        }
        writefile(bgcfg_path_, root.toStyledString(), false);
        invalidate_cache_unlocked(bg);
        load_bgcfg_unlocked();
    }
    cq_send(conf.p, fmt::format("已更新 {} 的 {} = {}", bg, field, value),
            conf);
    send_preview(bg, conf);
}

void dateimg::cmd_delbg(const std::string &arg, const msg_meta &conf) {
    const std::string bg = resolve_bg(arg);
    if (bg.empty()) {
        cq_send(conf.p, "没有这个背景（用 dateimg.list 查看）。", conf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (root.isObject() && root["backgrounds"].isObject()) {
            root["backgrounds"].removeMember(bg);
        }
        writefile(bgcfg_path_, root.toStyledString(), false);
        std::error_code ec;
        fs::remove(fs::path(bg_dir_) / bg, ec);
        invalidate_cache_unlocked(bg);
        load_bgcfg_unlocked();
    }
    cq_send(conf.p, "已删除背景: " + bg, conf);
}

void dateimg::cmd_rename(const std::string &arg, const msg_meta &conf) {
    std::istringstream iss(arg);
    std::string ref, newname;
    iss >> ref >> newname;
    if (ref.empty() || newname.empty()) {
        cq_send(conf.p, "用法: dateimg.rename <序号|名字> <新名字>", conf);
        return;
    }
    const std::string bg = resolve_bg(ref);
    if (bg.empty()) {
        cq_send(conf.p, "没有这个背景（用 dateimg.list 查看）。", conf);
        return;
    }
    const std::string ext = fs::path(bg).extension().string();
    const std::string newkey = dutil::safe_stem(newname) + ext;
    if (newkey == bg) {
        cq_send(conf.p, "名字未变。", conf);
        return;
    }
    if (fs::exists(fs::path(bg_dir_) / newkey)) {
        cq_send(conf.p, "目标名字已存在: " + newkey, conf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::error_code ec;
        fs::rename(fs::path(bg_dir_) / bg, fs::path(bg_dir_) / newkey, ec);
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (root.isObject() && root["backgrounds"].isObject() &&
            root["backgrounds"].isMember(bg)) {
            root["backgrounds"][newkey] = root["backgrounds"][bg];
            root["backgrounds"].removeMember(bg);
        }
        writefile(bgcfg_path_, root.toStyledString(), false);
        for (auto &kv : subs_) {
            if (kv.second == bg) {
                kv.second = newkey;
            }
        }
        save_subs_unlocked();
        invalidate_cache_unlocked(bg);
        load_bgcfg_unlocked();
    }
    cq_send(conf.p, "已重命名: " + bg + " -> " + newkey, conf);
}

void dateimg::cmd_mode(const std::string &arg, const msg_meta &conf) {
    std::string m = trim(arg);
    std::transform(m.begin(), m.end(), m.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (m != "gif" && m != "png") {
        std::string cur;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cur = mode_;
        }
        cq_send(conf.p, "用法: dateimg.mode <gif|png>  当前: " + cur, conf);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        Json::Value root = string_to_json(readfile(bgcfg_path_, "{}"));
        if (!root.isObject()) {
            root = Json::Value(Json::objectValue);
        }
        root["mode"] = m;
        writefile(bgcfg_path_, root.toStyledString(), false);
        load_bgcfg_unlocked();
    }
    cq_send(
        conf.p,
        "输出模式已设为: " + m +
            (m == "gif" ? "（单帧gif/贴纸，透明背景）" : "（png/普通图片）"),
        conf);
}

// ===================== blessing sentences =====================

void dateimg::cmd_msg(const std::string &arg, const msg_meta &conf) {
    std::istringstream iss(arg);
    std::string sub;
    iss >> sub;
    std::string rest;
    std::getline(iss, rest);
    rest = trim(rest);

    if (sub.empty() || sub == "list") {
        std::string out;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_messages_unlocked();
            std::ostringstream oss;
            oss << "祝福语 " << (msg_enabled_ ? "[开]" : "[关]") << " 共"
                << sentences_.size() << "条";
            oss << "\n前缀: " << msg_prefix_;
            oss << "\n引导: " << (msg_lead_.empty() ? "(空)" : msg_lead_);
            for (size_t i = 0; i < sentences_.size(); ++i) {
                oss << "\n[" << i << "] " << sentences_[i];
            }
            out = oss.str();
        }
        cq_send(conf.p, out, conf);
        return;
    }
    if (sub == "on" || sub == "off") {
        {
            std::lock_guard<std::mutex> lock(mu_);
            Json::Value root = string_to_json(readfile(msgs_path_, "{}"));
            if (!root.isObject()) {
                root = Json::Value(Json::objectValue);
            }
            root["enabled"] = (sub == "on");
            writefile(msgs_path_, root.toStyledString(), false);
            load_messages_unlocked();
        }
        cq_send(conf.p,
                std::string("每日祝福语已") + (sub == "on" ? "开启" : "关闭"),
                conf);
        return;
    }
    if (sub == "prefix" || sub == "lead") {
        {
            std::lock_guard<std::mutex> lock(mu_);
            Json::Value root = string_to_json(readfile(msgs_path_, "{}"));
            if (!root.isObject()) {
                root = Json::Value(Json::objectValue);
            }
            root[sub] = rest;
            writefile(msgs_path_, root.toStyledString(), false);
            load_messages_unlocked();
        }
        cq_send(conf.p,
                "已设置 " + sub + " = " + (rest.empty() ? "(空)" : rest), conf);
        return;
    }
    if (sub == "add") {
        if (rest.empty()) {
            cq_send(conf.p, "用法: dateimg.msg add <句子>", conf);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            Json::Value root = string_to_json(readfile(msgs_path_, "{}"));
            if (!root.isObject()) {
                root = Json::Value(Json::objectValue);
            }
            if (!root["sentences"].isArray()) {
                root["sentences"] = Json::Value(Json::arrayValue);
            }
            root["sentences"].append(rest);
            writefile(msgs_path_, root.toStyledString(), false);
            load_messages_unlocked();
        }
        cq_send(conf.p, "已添加。", conf);
        return;
    }
    if (sub == "del") {
        int idx = -1;
        try {
            idx = std::stoi(rest);
        } catch (...) {
        }
        if (idx < 0) {
            cq_send(conf.p, "用法: dateimg.msg del <序号>", conf);
            return;
        }
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Json::Value root = string_to_json(readfile(msgs_path_, "{}"));
            if (root["sentences"].isArray() &&
                idx < static_cast<int>(root["sentences"].size())) {
                Json::Value removed;
                root["sentences"].removeIndex(
                    static_cast<Json::ArrayIndex>(idx), &removed);
                writefile(msgs_path_, root.toStyledString(), false);
                load_messages_unlocked();
                ok = true;
            }
        }
        cq_send(conf.p, ok ? "已删除。" : "序号无效。", conf);
        return;
    }
    cq_send(conf.p,
            "用法: dateimg.msg [list | add <句子> | del <序号> | on | off | "
            "prefix <文字> | lead <文字>]",
            conf);
}

// ===================== help =====================

std::string dateimg::detailed_help(help_level_t level) {
    std::string s =
        "每日日期图 (dateimg):\n"
        "每天 00:00 给已启用的群随机推送写有当天日期(含农历)的图片。\n"
        "默认关闭，需群管理开启。\n"
        "dateimg.list - 查看背景(带序号)/字体/模式与本群状态\n"
        "date.list / date.help - 本群生日表(生日会随图一起推送)\n"
        "dateimg.help - 帮助";
    if (level != help_level_t::public_only) {
        s += "\n[管理] dateimg.enable / dateimg.disable - 开启/关闭本群\n"
             "[管理] dateimg.bg <序号|名字|random> - 指定本群背景\n"
             "[OP] dateimg.test - (私聊)合并转发预览全部背景\n"
             "[OP] dateimg.test <序号|名字> - (私聊)预览指定背景\n"
             "[OP] dateimg.addbg <名字> +图片 - (私聊)上传背景\n"
             "[OP] dateimg.setcfg <序号|名字> "
             "<x|y|font_size|rotation|color|font|"
             "layout> <值> - 调整并即时预览\n"
             "[OP] dateimg.rename <序号|名字> <新名字> - 重命名背景\n"
             "[OP] dateimg.delbg <序号|名字> - (私聊)删除背景\n"
             "[OP] dateimg.mode <gif|png> - 切换输出格式\n"
             "[OP] dateimg.msg [list|add|del|on|off|prefix|lead] - "
             "每日祝福语管理";
    }
    return s;
}
