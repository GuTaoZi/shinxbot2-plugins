#include "dateimg.h"

#include "dateimg_util.h"
#include "lunar.h"
#include "utils.h"

#include <algorithm>
#include <ctime>
#include <fmt/format.h>
#include <sstream>

// ===================== per-group birthday store =====================

void dateimg::load_birthdays_unlocked() {
    std::map<groupid_t, std::vector<bday_entry>> bd;
    try {
        Json::Value root = string_to_json(readfile(bdays_path_, "{}"));
        if (root.isObject()) {
            for (const auto &gk : root.getMemberNames()) {
                const Json::Value &arr = root[gk];
                if (!arr.isArray()) {
                    continue;
                }
                groupid_t g;
                try {
                    g = static_cast<groupid_t>(std::stoull(gk));
                } catch (...) {
                    continue;
                }
                std::vector<bday_entry> v;
                for (const auto &e : arr) {
                    if (!e.isObject()) {
                        continue;
                    }
                    bday_entry b;
                    b.who = e.get("who", "").asString();
                    b.lunar = e.get("lunar", false).asBool();
                    b.mm = e.get("mm", 0).asInt();
                    b.dd = e.get("dd", 0).asInt();
                    if (!b.who.empty() && b.mm >= 1 && b.mm <= 12 &&
                        b.dd >= 1 && b.dd <= 31) {
                        v.push_back(b);
                    }
                }
                bd[g] = std::move(v);
            }
        }
    } catch (const std::exception &e) {
        set_global_log(
            LOG::WARNING,
            std::string("dateimg: bad birthdays.json, keeping old: ") +
                e.what());
        return;
    }
    birthdays_.swap(bd);
}

void dateimg::save_birthdays_unlocked() const {
    Json::Value root(Json::objectValue);
    for (const auto &kv : birthdays_) {
        Json::Value arr(Json::arrayValue);
        for (const auto &b : kv.second) {
            Json::Value e(Json::objectValue);
            e["who"] = b.who;
            e["lunar"] = b.lunar;
            e["mm"] = b.mm;
            e["dd"] = b.dd;
            arr.append(e);
        }
        root[std::to_string(kv.first)] = arr;
    }
    writefile(bdays_path_, root.toStyledString(), false);
}

std::vector<std::string>
dateimg::birthday_wishes_unlocked(groupid_t g, const std::tm &lt) const {
    std::vector<std::string> out;
    auto it = birthdays_.find(g);
    if (it == birthdays_.end()) {
        return out;
    }
    const int sm = lt.tm_mon + 1, sd = lt.tm_mday, sy = lt.tm_year + 1900;
    lunar::LunarDate ld = lunar::solar2lunar(sy, sm, sd);
    for (const auto &b : it->second) {
        bool hit = false;
        if (b.lunar) {
            // match on lunar month/day (ignore leap months)
            hit = ld.valid && !ld.isLeap && b.mm == ld.month && b.dd == ld.day;
        } else {
            hit = (b.mm == sm && b.dd == sd);
        }
        if (hit) {
            out.push_back(b.who);
        }
    }
    return out;
}

// ===================== date.* commands (group, member-editable)
// =====================

void dateimg::cmd_date(const std::string &raw, const msg_meta &conf) {
    std::istringstream iss(raw);
    std::string cmd; // e.g. "date.add"
    iss >> cmd;
    std::string args;
    std::getline(iss, args);
    args = trim(args);

    const bool can_manage = conf.p->is_op(conf.user_id) ||
                            is_group_op(conf.p, conf.group_id, conf.user_id);

    if (cmd == "date.help") {
        cq_send(
            conf.p,
            "生日提醒 (本群):\n"
            "date.list - 查看本群生日表\n"
            "date.add <日期> <祝福语> - 添加(成员可用)。日期: 阳历 M.D；农历 "
            "农历M.D\n"
            "  例: date.add 8.10 祝小白生日快乐！\n"
            "  例: date.add 农历1.13 祝小骑士生日快乐！\n"
            "date.del <祝福语/关键词> - 删除(仅管理)\n"
            "每天 00:00 随日期图一起推送当天生日祝福(需本群已启用 dateimg)。",
            conf);
        return;
    }
    if (cmd == "date.list") {
        std::string out;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_birthdays_unlocked();
            auto it = birthdays_.find(conf.group_id);
            std::ostringstream oss;
            if (it == birthdays_.end() || it->second.empty()) {
                oss << "本群生日表空空的";
            } else {
                oss << "本群生日表 (" << it->second.size() << "):";
                for (const auto &b : it->second) {
                    oss << "\n"
                        << (b.lunar ? "[农历] " : "[阳历] ")
                        << fmt::format("{}.{} ", b.mm, b.dd) << b.who;
                }
            }
            out = oss.str();
        }
        cq_send(conf.p, out, conf);
        return;
    }
    if (cmd == "date.add") {
        std::istringstream a(args);
        std::string datetok;
        a >> datetok;
        std::string who;
        std::getline(a, who);
        who = trim(who);
        if (datetok.empty() || who.empty()) {
            cq_send(conf.p,
                    "用法: date.add <日期> <祝福语>，日期如 8.10 或 农历1.13",
                    conf);
            return;
        }
        if (who.size() > 200) {
            cq_send(conf.p, "祝福语太长了喵。", conf);
            return;
        }
        bool lunar = false;
        std::string dt = datetok;
        if (dt.rfind("农历", 0) == 0) {
            lunar = true;
            dt = dt.substr(std::string("农历").size());
        } else if (dt.rfind("阴历", 0) == 0) {
            lunar = true;
            dt = dt.substr(std::string("阴历").size());
        } else if (!dt.empty() && (dt[0] == 'L' || dt[0] == 'l')) {
            lunar = true;
            dt = dt.substr(1);
        }
        int mm = 0, dd = 0;
        if (!dutil::parse_md(dt, mm, dd) || mm < 1 || mm > 12 || dd < 1 ||
            dd > 31) {
            cq_send(conf.p, "日期格式错误，用 M.D（如 8.10）或 农历M.D。",
                    conf);
            return;
        }
        bool full = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_birthdays_unlocked();
            auto &v = birthdays_[conf.group_id];
            if (v.size() >= 300) {
                full = true;
            } else {
                v.push_back({who, lunar, mm, dd});
                save_birthdays_unlocked();
            }
        }
        if (full) {
            cq_send(conf.p, "本群生日表已满(300)。", conf);
            return;
        }
        cq_send(conf.p,
                fmt::format("已添加{} {}.{}：{}", lunar ? "(农历)" : "(阳历)",
                            mm, dd, who),
                conf);
        return;
    }
    if (cmd == "date.del") {
        if (!can_manage) {
            cq_send(conf.p, "只有群管理或 bot 管理可以删除哦。", conf);
            return;
        }
        const std::string key = trim(args);
        if (key.empty()) {
            cq_send(conf.p, "用法: date.del <祝福语或关键词>", conf);
            return;
        }
        int removed = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_birthdays_unlocked();
            auto it = birthdays_.find(conf.group_id);
            if (it != birthdays_.end()) {
                auto &v = it->second;
                const size_t before = v.size();
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [&](const bday_entry &b) {
                                           return b.who == key ||
                                                  b.who.find(key) !=
                                                      std::string::npos;
                                       }),
                        v.end());
                removed = static_cast<int>(before - v.size());
                if (removed > 0) {
                    save_birthdays_unlocked();
                }
            }
        }
        cq_send(conf.p,
                removed > 0 ? fmt::format("删除了 {} 条。", removed)
                            : "找不到匹配的条目。",
                conf);
        return;
    }
    cq_send(conf.p, "未知命令，请用 date.help", conf);
}
