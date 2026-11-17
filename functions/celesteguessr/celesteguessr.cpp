#include "celesteguessr.h"

#include "utils.h"

#include <Magick++.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using clock_t_ = std::chrono::steady_clock;

namespace {

const char *FONT_B = "Noto-Sans-CJK-SC-Bold";

std::string lower_ascii(std::string s)
{
    for (auto &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool all_digits(const std::string &s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string replace_all(std::string s, const std::string &from, const std::string &to)
{
    for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size())) {
        s.replace(i, from.size(), to);
    }
    return s;
}

std::vector<std::string> split_ws(const std::string &s)
{
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string t; in >> t;) {
        out.push_back(t);
    }
    return out;
}

std::string fmt_secs(double s)
{
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << s;
    return o.str();
}

void text(Magick::Image &img, double pt, const Magick::Color &fill, double x, double y, const std::string &s)
{
    std::vector<Magick::Drawable> d{Magick::DrawableFont(FONT_B), Magick::DrawablePointSize(pt),
                                    Magick::DrawableFillColor(fill), Magick::DrawableStrokeColor("none"),
                                    Magick::DrawableText(x, y, s, "UTF-8")};
    img.draw(d);
}

// A numbered disc centred on (cx, cy).
void badge(Magick::Image &img, double cx, double cy, double r, const Magick::Color &bg, const std::string &label)
{
    img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor(bg), Magick::DrawableStrokeColor("white"),
                                           Magick::DrawableStrokeWidth(2), Magick::DrawableCircle(cx, cy, cx + r, cy)});
    const double pt = r * 1.15;
    Magick::TypeMetric m;
    Magick::Image probe(Magick::Geometry(1, 1), "white");
    probe.font(FONT_B);
    probe.fontPointsize(pt);
    probe.fontTypeMetrics(label, &m);
    text(img, pt, Magick::Color("white"), cx - m.textWidth() / 2, cy + 0.37 * pt, label);
}

} // namespace

const celesteguessr::room_t *celesteguessr::side_t::room(int num) const
{
    for (const auto &r : rooms) {
        if (r.num == num) {
            return &r;
        }
    }
    return nullptr;
}

const celesteguessr::room_t *celesteguessr::side_t::room_named(const std::string &name) const
{
    std::string n = lower_ascii(name);
    if (n.rfind("lvl_", 0) == 0) {
        n = n.substr(4);
    }
    for (const auto &r : rooms) {
        if (lower_ascii(r.name) == n) {
            return &r;
        }
    }
    return nullptr;
}

celesteguessr::celesteguessr()
{
    sync_dirs(nullptr);
}

void celesteguessr::sync_dirs(const bot *p)
{
    const std::string cfg = p ? p->getConfigDir() : bot_config_path("");
    const std::string res = p ? p->getResourceDir() : bot_resource_path("");
    if (cfg == config_dir_ && res == resource_dir_ && !sides_.empty()) {
        return;
    }
    config_dir_ = cfg;
    // absolute: NapCat reads the file:// pictures from its own working directory (the bot's is "./resource")
    resource_dir_ = fs::absolute(res).lexically_normal().string();
    data_dir_ = (fs::path(resource_dir_) / "celesteguessr").string();
    load_data();
}

bool celesteguessr::load_data()
{
    sides_.clear();
    pool_.clear();
    std::error_code ec;
    if (!fs::is_directory(data_dir_, ec)) {
        return false;
    }
    for (const auto &e : fs::directory_iterator(data_dir_, ec)) {
        const fs::path js = e.path() / "side.json";
        if (!e.is_directory() || !fs::exists(js)) {
            continue;
        }
        Json::Value J = string_to_json(readfile(js.string(), "{}"));
        side_t s;
        s.key = J["key"].asString();
        s.name_zh = J["name_zh"].asString();
        s.name_en = J["name_en"].asString();
        s.dir = e.path().string();
        s.overview = (e.path() / J["overview"]["file"].asString()).string();
        s.scale = J["overview"]["scale"].asDouble();
        s.ox = J["overview"]["ox"].asDouble();
        s.oy = J["overview"]["oy"].asDouble();
        for (const auto &r : J["rooms"]) {
            s.rooms.push_back({r["num"].asInt(), r["name"].asString(), r["x"].asDouble(), r["y"].asDouble(),
                               r["w"].asDouble(), r["h"].asDouble()});
        }
        for (const auto &t : J["shots"]) {
            s.shots.push_back({(e.path() / t["file"].asString()).string(), t["room"].asInt(), t["x"].asDouble(),
                               t["y"].asDouble()});
        }
        if (s.key.empty() || s.rooms.empty() || s.shots.empty()) {
            continue;
        }
        for (size_t i = 0; i < s.shots.size(); ++i) {
            pool_.push_back({s.key, i});
        }
        sides_[s.key] = std::move(s);
    }
    set_global_log(LOG::INFO, "celesteguessr: " + std::to_string(sides_.size()) + " sides, " +
                                  std::to_string(pool_.size()) + " screenshots from " + data_dir_);
    return !pool_.empty();
}

std::string celesteguessr::scope(const msg_meta &conf) const
{
    return conf.message_type == "group" ? "g" + std::to_string(conf.group_id) : "u" + std::to_string(conf.user_id);
}

std::string celesteguessr::scores_path() const
{
    return (fs::path(config_dir_) / "features/celesteguessr/scores.json").string();
}

// "1a" -> "1A", "8" -> "8A", "p" / "序幕" -> "0", "e" / "尾声" -> "E", "9" / "再见" -> "9"; "" if not a side.
std::string celesteguessr::side_key(std::string s)
{
    s = lower_ascii(trim(s));
    static const std::map<std::string, std::string> alias = {
        {"0", "0"},  {"0a", "0"},  {"p", "0"},  {"prologue", "0"},  {"序章", "0"},    {"序幕", "0"},
        {"e", "E"},  {"ea", "E"},  {"7'", "E"}, {"epilogue", "E"},  {"尾声", "E"},    {"9", "9"},
        {"9a", "9"}, {"fw", "9"},  {"farewell", "9"}, {"再见", "9"}, {"告别", "9"},
    };
    auto it = alias.find(s);
    if (it != alias.end()) {
        return it->second;
    }
    if (s.size() == 1 && s[0] >= '1' && s[0] <= '8') {
        return s + "A";
    }
    if (s.size() == 2 && s[0] >= '1' && s[0] <= '8' && (s[1] == 'a' || s[1] == 'b' || s[1] == 'c')) {
        return std::string(1, s[0]) + static_cast<char>(std::toupper(static_cast<unsigned char>(s[1])));
    }
    return "";
}

bool celesteguessr::parse_guess(const std::string &body, std::string &side, int &room, std::string &err) const
{
    std::string b = body;
    for (const char *sep : {"-", "#", "/", ",", "，", "号", "房间", "　"}) {
        b = replace_all(b, sep, " ");
    }
    std::vector<std::string> t = split_ws(b);
    std::string room_tok;
    if (t.size() >= 2) {
        side = side_key(t[0]);
        room_tok = t[1];
    }
    else if (t.size() == 1) { // "1a19": the longest leading side key, the rest is the room
        side.clear();
        for (size_t n = std::min<size_t>(t[0].size() - 1, 8); n >= 1 && side.empty(); --n) {
            const std::string k = side_key(t[0].substr(0, n));
            if (!k.empty()) {
                side = k;
                room_tok = t[0].substr(n);
            }
        }
    }
    if (side.empty() || room_tok.empty()) {
        err = "格式：*cg <章节> <房间号>，如 *cg 1A 19";
        return false;
    }
    auto it = sides_.find(side);
    if (it == sides_.end()) {
        std::string keys;
        for (const auto &kv : sides_) {
            keys += (keys.empty() ? "" : " ") + kv.first;
        }
        err = "题库里还没有 " + side + "（现有：" + keys + "）";
        return false;
    }
    const room_t *r = all_digits(room_tok) ? it->second.room(std::stoi(room_tok.substr(0, 6)))
                                           : it->second.room_named(room_tok);
    if (r == nullptr) {
        err = side + " 没有房间 " + room_tok + "（1-" + std::to_string(it->second.rooms.size()) +
              "，见 *cg map " + side + "）";
        return false;
    }
    room = r->num;
    return true;
}

// "1A" -> "1", "8C" -> "8", "0" / "E" / "9" stay.
std::string celesteguessr::chapter_of(const std::string &key)
{
    return key.size() == 2 ? key.substr(0, 1) : key;
}

// CelesteGuessr's scale (its server settings: max_points 5000, wrong_side_max_points 100; its curve is not public),
// in the user's tiers (2026-11-17: "the farther, the lower score ... shrink like exponential, maybe bullseye 3, then
// close 2 and same chapter 1"): the right room 5000; another room of the right side 300 + 3700·e^(-d/640), d = world
// px from the spot to that room (a screen is 320 wide: next door ~3500, two screens ~1650, far away 300 - the right
// chapter always beats a wrong one); the right chapter's other side 100; another chapter 0.
int celesteguessr::score_for(const side_t &answer_side, const shot_t &shot, const std::string &side, const room_t *room)
{
    if (side != answer_side.key) {
        return chapter_of(side) == chapter_of(answer_side.key) ? 100 : 0;
    }
    if (room == nullptr) {
        return 0;
    }
    if (room->num == shot.room) {
        return 5000;
    }
    const double dx = std::max({room->x - shot.x, 0.0, shot.x - (room->x + room->w)});
    const double dy = std::max({room->y - shot.y, 0.0, shot.y - (room->y + room->h)});
    return static_cast<int>(std::lround(300.0 + 3700.0 * std::exp(-std::hypot(dx, dy) / 640.0)));
}

// bullseye / close / the right side / the right chapter's other side / missed
std::string celesteguessr::tier(int score)
{
    if (score >= 5000) {
        return "🎯正中";
    }
    if (score >= 1500) {
        return "很近";
    }
    if (score >= 300) {
        return "同一面";
    }
    if (score >= 100) {
        return "同章节·另一面";
    }
    return "没猜中";
}

void celesteguessr::start_round(const msg_meta &conf, int seconds)
{
    const std::string sc = scope(conf);
    auto &r = rounds_[sc];
    if (r.active) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(r.deadline - clock_t_::now()).count();
        const auto &s = sides_.at(r.side);
        conf.p->cq_send("[CQ:image,file=file://" + s.shots[r.shot].file + ",id=40000]本轮还在进行，剩 " +
                            std::to_string(std::max<long long>(0, left)) + " 秒。*cg <章节> <房间号> 作答",
                        conf);
        return;
    }
    if (pool_.empty()) {
        conf.p->cq_send("CelesteGuessr 题库是空的（" + data_dir_ + "）", conf);
        return;
    }
    auto &recent = recent_[sc];
    std::pair<std::string, size_t> pick;
    for (int tries = 0; tries < 50; ++tries) {
        pick = pool_[get_random(static_cast<int>(pool_.size()))];
        if (std::find(recent.begin(), recent.end(), pick) == recent.end()) {
            break;
        }
    }
    recent.push_back(pick);
    while (recent.size() > std::min(kRecent, pool_.size() / 2)) {
        recent.pop_front();
    }
    r = round_t{};
    r.active = true;
    r.side = pick.first;
    r.shot = pick.second;
    r.seconds = seconds;
    r.started = clock_t_::now();
    r.deadline = r.started + std::chrono::seconds(seconds);
    r.starter = conf.user_id;
    r.message_type = conf.message_type;
    r.user_id = conf.user_id;
    r.group_id = conf.group_id;
    r.last_activity = r.started;

    std::string keys;
    for (const auto &kv : sides_) {
        keys += (keys.empty() ? "" : " ") + kv.first;
    }
    conf.p->cq_send("[CQ:image,file=file://" + sides_.at(r.side).shots[r.shot].file +
                        ",id=40000]CelesteGuessr：这张截图在哪里？\n"
                        "① *cg 1A 先选章节（会发这一章的地图）\n"
                        "② *cg 19 再按地图上的号码猜房间（每人一次）\n"
                        "熟练的话一步：*cg 1A 19 · *cg check 看进度\n"
                        "限时 " + std::to_string(seconds) + " 秒 · 题库：" + keys,
                    conf);
}

void celesteguessr::pick_chapter(const msg_meta &conf, round_t &r, const std::string &side)
{
    auto it = sides_.find(side);
    if (it == sides_.end()) {
        std::string keys;
        for (const auto &kv : sides_) {
            keys += (keys.empty() ? "" : " ") + kv.first;
        }
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) + "]题库里还没有 " + side + "（现有：" +
                            keys + "）",
                        conf);
        return;
    }
    r.picks[conf.user_id] = side;
    r.last_activity = clock_t_::now();
    const side_t &s = it->second;
    if (r.maps_sent.insert(side).second) {
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) + "]" + s.key + " " + s.name_zh + "（" +
                            std::to_string(s.rooms.size()) + " 个房间）*cg <房间号> 作答[CQ:image,file=file://" +
                            s.overview + ",id=40000]",
                        conf);
    }
    else {
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) + "]已选 " + s.key + "，地图在上面，*cg <房间号> 作答",
                        conf);
    }
}

void celesteguessr::record_guess(const msg_meta &conf, round_t &r, const std::string &side, int room)
{
    const side_t &ans = sides_.at(r.side);
    guess_t g;
    g.user = conf.user_id;
    g.name = display_name_in_group(conf, conf.user_id);
    g.side = side;
    g.room = room;
    g.score = score_for(ans, ans.shots[r.shot], side, sides_.at(side).room(room));
    g.secs = std::chrono::duration<double>(clock_t_::now() - r.started).count();
    r.guesses.push_back(g);
    r.picks[conf.user_id] = side;
    r.last_activity = clock_t_::now();
    react(conf, "9989", "收到：" + side + " " + std::to_string(room));
}

void celesteguessr::send_check(const msg_meta &conf)
{
    auto it = rounds_.find(scope(conf));
    if (it == rounds_.end() || !it->second.active) {
        conf.p->cq_send("现在没有进行中的回合，*cg 开始一轮", conf);
        return;
    }
    const round_t &r = it->second;
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(r.deadline - clock_t_::now()).count();
    std::string done, waiting;
    for (const auto &g : r.guesses) {
        done += (done.empty() ? "" : "、") + g.name;
    }
    for (const auto &kv : r.picks) {
        bool guessed = false;
        for (const auto &g : r.guesses) {
            guessed |= g.user == kv.first;
        }
        if (!guessed) {
            waiting += (waiting.empty() ? "" : "、") + display_name_in_group(conf, kv.first);
        }
    }
    conf.p->cq_send("CelesteGuessr 进行中，剩 " + std::to_string(std::max<long long>(0, left)) + " 秒\n已作答（" +
                        std::to_string(r.guesses.size()) + "）：" + (done.empty() ? "无" : done) +
                        (waiting.empty() ? "" : "\n选了章节还没猜：" + waiting) +
                        "\n所有选了章节的人都猜完后 10 秒自动揭晓",
                    conf);
}

std::string celesteguessr::draw_reveal(const std::string &sc, const round_t &r) const
{
    const side_t &s = sides_.at(r.side);
    const shot_t &shot = s.shots[r.shot];
    Magick::Image img(s.overview);
    auto X = [&](double wx) { return (wx - s.ox) * s.scale; };
    auto Y = [&](double wy) { return (wy - s.oy) * s.scale; };

    // the players' rooms (right side only), numbered by their place in the results
    std::vector<guess_t> order = r.guesses;
    std::stable_sort(order.begin(), order.end(), [](const guess_t &a, const guess_t &b) {
        return a.score != b.score ? a.score > b.score : a.secs < b.secs;
    });
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i].side != s.key) {
            continue;
        }
        const room_t *rm = s.room(order[i].room);
        if (rm == nullptr) {
            continue;
        }
        img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor("none"), Magick::DrawableStrokeColor("#00c8ff"),
                                               Magick::DrawableStrokeWidth(4),
                                               Magick::DrawableRectangle(X(rm->x), Y(rm->y), X(rm->x + rm->w),
                                                                         Y(rm->y + rm->h))});
        // top-right corner: the room's own number badge is top-left
        badge(img, X(rm->x + rm->w) - 18, Y(rm->y) + 18, 14, Magick::Color("#0090c0"), std::to_string(i + 1));
    }
    // the answer: the screenshot's view and its centre
    const double cx = X(shot.x), cy = Y(shot.y), hw = 160 * s.scale, hh = 90 * s.scale;
    img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor("none"), // a frame only: the room's shape stays visible
                                           Magick::DrawableStrokeColor("#ffd600"), Magick::DrawableStrokeWidth(5),
                                           Magick::DrawableRectangle(cx - hw, cy - hh, cx + hw, cy + hh)});
    img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor("#ffd600"), Magick::DrawableStrokeColor("black"),
                                           Magick::DrawableStrokeWidth(3), Magick::DrawableCircle(cx, cy, cx + 11, cy)});

    const fs::path dir = fs::path(resource_dir_) / "celesteguessr_tmp";
    fs::create_directories(dir);
    const std::string out = (dir / (sc + "_reveal.jpg")).string();
    img.quality(90);
    img.write(out);
    return out;
}

void celesteguessr::save_scores(const std::string &sc, const round_t &r)
{
    const std::string path = scores_path();
    fs::create_directories(fs::path(path).parent_path());
    Json::Value root = string_to_json(readfile(path, "{}"));
    for (const auto &g : r.guesses) {
        Json::Value &u = root[sc][std::to_string(g.user)];
        u["name"] = g.name;
        u["total"] = u["total"].asInt64() + g.score;
        u["rounds"] = u["rounds"].asInt() + 1;
        u["best"] = std::max(u["best"].asInt(), g.score);
        u["perfect"] = u["perfect"].asInt() + (g.score >= 5000 ? 1 : 0);
    }
    writefile(path, root.toStyledString(), false);
}

void celesteguessr::finish_round(const std::string &sc, round_t &r, bot *p, const std::string &why)
{
    r.active = false;
    msg_meta conf(r.message_type, r.user_id, r.group_id, 0, p);
    const side_t &s = sides_.at(r.side);
    const shot_t &shot = s.shots[r.shot];
    const room_t *ans = s.room(shot.room);

    std::vector<guess_t> order = r.guesses;
    std::stable_sort(order.begin(), order.end(), [](const guess_t &a, const guess_t &b) {
        return a.score != b.score ? a.score > b.score : a.secs < b.secs;
    });
    std::string msg = why + "！答案：" + s.key + " " + s.name_zh + " · " +
                      std::to_string(shot.room) + " 号房间" + (ans ? "（" + ans->name + "）" : "");
    if (order.empty()) {
        msg += "\n没有人作答。";
    }
    for (size_t i = 0; i < order.size(); ++i) {
        const auto &g = order[i];
        msg += "\n" + std::to_string(i + 1) + ". " + g.name + "  " + std::to_string(g.score) + " 分 " + tier(g.score) +
               "（" + g.side + " " + std::to_string(g.room) + "，" + fmt_secs(g.secs) + " 秒）";
    }
    std::string img;
    try {
        img = draw_reveal(sc, r);
    }
    catch (const std::exception &e) {
        set_global_log(LOG::ERROR, std::string("celesteguessr: reveal: ") + e.what());
    }
    p->cq_send(msg + (img.empty() ? "" : "[CQ:image,file=file://" + img + ",id=40000]"), conf);
    if (!r.guesses.empty()) {
        save_scores(sc, r);
    }
}

void celesteguessr::send_rank(const msg_meta &conf)
{
    Json::Value root = string_to_json(readfile(scores_path(), "{}"));
    const Json::Value &g = root[scope(conf)];
    struct row {
        std::string name;
        int64_t total;
        int rounds, perfect;
    };
    std::vector<row> rows;
    for (const auto &k : g.getMemberNames()) {
        rows.push_back({g[k]["name"].asString(), g[k]["total"].asInt64(), g[k]["rounds"].asInt(), g[k]["perfect"].asInt()});
    }
    std::sort(rows.begin(), rows.end(), [](const row &a, const row &b) { return a.total > b.total; });
    std::string msg = "CelesteGuessr 排行";
    if (rows.empty()) {
        msg += "\n还没有成绩，*cg 开始一轮";
    }
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        const auto &x = rows[i];
        msg += "\n" + std::to_string(i + 1) + ". " + x.name + "  " + std::to_string(x.total) + " 分（" +
               std::to_string(x.rounds) + " 轮，平均 " + std::to_string(x.rounds ? x.total / x.rounds : 0) + "，满分 " +
               std::to_string(x.perfect) + " 次）";
    }
    conf.p->cq_send(msg, conf);
}

void celesteguessr::send_map(const msg_meta &conf, const std::string &arg)
{
    std::string key = side_key(arg);
    if (key.empty()) {
        auto it = rounds_.find(scope(conf));
        if (arg.empty() && sides_.size() == 1) {
            key = sides_.begin()->first;
        }
        else {
            std::string keys;
            for (const auto &kv : sides_) {
                keys += (keys.empty() ? "" : " ") + kv.first;
            }
            conf.p->cq_send("*cg map <章节>，现有：" + keys, conf);
            (void)it;
            return;
        }
    }
    auto it = sides_.find(key);
    if (it == sides_.end()) {
        conf.p->cq_send("题库里还没有 " + key, conf);
        return;
    }
    conf.p->cq_send(it->second.key + " " + it->second.name_zh + "（" + std::to_string(it->second.rooms.size()) +
                        " 个房间）[CQ:image,file=file://" + it->second.overview + ",id=40000]",
                    conf);
}

void celesteguessr::react(const msg_meta &conf, const std::string &emoji, const std::string &fallback) const
{
    bool ok = false;
    try {
        Json::Value J;
        J["message_id"] = conf.message_id;
        J["emoji_id"] = emoji;
        ok = string_to_json(conf.p->cq_send("set_msg_emoji_like", J))["status"].asString() == "ok";
    }
    catch (...) {
    }
    if (!ok && !fallback.empty()) {
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) + "]" + fallback, conf);
    }
}

static std::string help_text()
{
    return "CelesteGuessr：一张截图，猜它在蔚蓝的哪个房间\n"
           "*cg：开始一轮（*cg 开始 60 = 限时 60 秒）\n"
           "*cg 1A：选章节，发这一章的地图（选完还能换）\n"
           "*cg 19：按地图上的号码猜房间（每人一次）；一步到位：*cg 1A 19\n"
           "*cg check：看谁猜了、还剩多久\n"
           "*cg map 1A：随时看某章地图 · *cg 结束：提前揭晓（发起人或管理）· *cg 排行：本群总分\n"
           "计分：正中 5000；同一面越远越少（隔壁约 3500，两屏外约 1650，最少 300）；同章节另一面 100；别的章节 0。"
           "时间到、或选了章节的人都猜完 10 秒后揭晓。\n"
           "章节：0 序幕，1A-7C，E 尾声，8A-8C 核心，9 再见（选了章节后纯数字是房间号；换到再见用 *cg fw）";
}

bool celesteguessr::check(std::string message, const msg_meta &conf)
{
    (void)conf;
    return cmd_match_prefix(trim(message), {"*cg"});
}

void celesteguessr::process(std::string message, const msg_meta &conf)
{
    std::lock_guard<std::mutex> lk(mu_);
    sync_dirs(conf.p);
    std::string body;
    if (!cmd_strip_prefix(trim(message), "*cg", body)) {
        return;
    }
    if (!body.empty() && body[0] == '.') { // *cg.help
        body = body.substr(1);
    }
    const std::vector<std::string> t = split_ws(body);
    const std::string cmd = t.empty() ? "" : lower_ascii(t[0]);
    const std::string arg = t.size() > 1 ? t[1] : "";

    if (cmd.empty() || cmd == "start" || cmd == "开始" || cmd == "s") {
        int secs = kDefaultSeconds;
        if (all_digits(arg)) {
            secs = std::clamp(std::stoi(arg.substr(0, 4)), kMinSeconds, kMaxSeconds);
        }
        start_round(conf, secs);
        return;
    }
    if (cmd == "help" || cmd == "帮助") {
        conf.p->cq_send(help_text(), conf);
        return;
    }
    if (cmd == "map" || cmd == "地图") {
        send_map(conf, arg);
        return;
    }
    if (cmd == "rank" || cmd == "排行") {
        send_rank(conf);
        return;
    }
    if (cmd == "check" || cmd == "进度" || cmd == "状态") {
        send_check(conf);
        return;
    }
    if (cmd == "reload") {
        if (conf.p->is_op(conf.user_id)) {
            load_data();
            conf.p->cq_send("CelesteGuessr：" + std::to_string(sides_.size()) + " 面，" +
                                std::to_string(pool_.size()) + " 张截图",
                            conf);
        }
        return;
    }
    const std::string sc = scope(conf);
    auto it = rounds_.find(sc);
    const bool active = it != rounds_.end() && it->second.active;
    if (cmd == "end" || cmd == "结束" || cmd == "giveup" || cmd == "放弃") {
        if (!active) {
            conf.p->cq_send("现在没有进行中的回合", conf);
            return;
        }
        const bool may = conf.user_id == it->second.starter || conf.p->is_op(conf.user_id) ||
                         (conf.message_type == "group" && is_group_op(conf.p, conf.group_id, conf.user_id));
        if (!may) {
            conf.p->cq_send("只有发起人或管理可以提前结束", conf);
            return;
        }
        finish_round(sc, it->second, conf.p, "本轮提前结束");
        return;
    }

    // a chapter pick or a guess
    if (!active) {
        conf.p->cq_send("现在没有进行中的回合，*cg 开始一轮", conf);
        return;
    }
    round_t &r = it->second;
    for (const auto &g : r.guesses) {
        if (g.user == conf.user_id) {
            react(conf, "10068", "你已经猜过了，等本轮结束吧");
            return;
        }
    }
    auto reply = [&](const std::string &s) {
        conf.p->cq_send("[CQ:reply,id=" + std::to_string(conf.message_id) + "]" + s, conf);
    };
    if (t.size() == 1) {
        const std::string &tok = t[0];
        auto pk = r.picks.find(conf.user_id);
        const side_t *picked = pk != r.picks.end() && sides_.count(pk->second) ? &sides_.at(pk->second) : nullptr;
        // after a pick a bare number is a room ("*cg 9" is room 9, not Farewell: *cg fw switches to Farewell)
        if (picked != nullptr && (all_digits(tok) || side_key(tok).empty())) {
            const room_t *rm = all_digits(tok) ? picked->room(std::stoi(tok.substr(0, 6))) : picked->room_named(tok);
            if (rm != nullptr) {
                record_guess(conf, r, picked->key, rm->num);
                return;
            }
            if (all_digits(tok)) {
                reply(picked->key + " 没有 " + tok + " 号房间（1-" + std::to_string(picked->rooms.size()) + "）");
                return;
            }
        }
        const std::string k = side_key(tok);
        if (!k.empty()) {
            pick_chapter(conf, r, k);
            return;
        }
        if (picked == nullptr && all_digits(tok)) {
            reply("先选章节：*cg 1A，再 *cg " + tok);
            return;
        }
    }
    std::string side, err;
    int room = 0;
    if (!parse_guess(body, side, room, err)) {
        reply(err);
        return;
    }
    record_guess(conf, r, side, room);
}

void celesteguessr::tick(bot *p)
{
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = clock_t_::now();
    for (auto &kv : rounds_) {
        round_t &r = kv.second;
        // early end: someone guessed, everyone who picked a chapter has guessed, and 10 s passed quietly
        bool all_in = r.active && !r.guesses.empty();
        for (const auto &pk : r.picks) {
            bool guessed = false;
            for (const auto &g : r.guesses) {
                guessed |= g.user == pk.first;
            }
            all_in &= guessed;
        }
        const bool all_done = all_in && now - r.last_activity >= std::chrono::seconds(10);
        if (r.active && (all_done || now >= r.deadline)) {
            try {
                finish_round(kv.first, kv.second, p, all_done && now < r.deadline ? "大家都猜完了" : "时间到");
            }
            catch (const std::exception &e) {
                kv.second.active = false;
                set_global_log(LOG::ERROR, std::string("celesteguessr: finish: ") + e.what());
            }
        }
    }
}

void celesteguessr::set_callback(std::function<void(std::function<void(bot *p)>)> f)
{
    f([this](bot *p) { tick(p); });
}

bool celesteguessr::reload(const msg_meta &conf)
{
    std::lock_guard<std::mutex> lk(mu_);
    config_dir_.clear();
    sync_dirs(conf.p);
    return !pool_.empty();
}

std::string celesteguessr::help()
{
    return "CelesteGuessr：根据截图猜蔚蓝的房间。帮助：*cg help";
}

DECLARE_FACTORY_FUNCTIONS(celesteguessr)
