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
    return !s.empty() && s.size() <= 6 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
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

std::string image(const std::string &file) { return "[CQ:image,file=file://" + file + ",id=40000]"; }

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

// ---- side data ---------------------------------------------------------------------------------------------------

const celesteguessr::room_t *celesteguessr::side_t::room(int num) const
{
    for (const auto &r : rooms) {
        if (r.num == num) {
            return &r;
        }
    }
    return nullptr;
}

const celesteguessr::room_t *celesteguessr::side_t::room_at(int part, int local) const
{
    for (const auto &r : rooms) {
        if (r.part == part && r.local == local) {
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

int celesteguessr::side_t::part_size(int part) const
{
    return static_cast<int>(std::count_if(rooms.begin(), rooms.end(), [&](const room_t &r) { return r.part == part; }));
}

std::string celesteguessr::side_t::label(int num) const
{
    const room_t *r = room(num);
    if (r == nullptr) {
        return key + " ?";
    }
    return maps.size() > 1 ? key + "-" + std::to_string(r->part) + " " + std::to_string(r->local)
                           : key + " " + std::to_string(r->local);
}

// ---- setup -------------------------------------------------------------------------------------------------------

celesteguessr::celesteguessr()
{
    sync_dirs(nullptr);
}

void celesteguessr::sync_dirs(const bot *p)
{
    const std::string cfg = p ? p->getConfigDir() : bot_config_path("");
    const std::string res = p ? p->getResourceDir() : bot_resource_path("");
    const std::string res_abs = fs::absolute(res).lexically_normal().string();
    if (cfg == config_dir_ && res_abs == resource_dir_ && !sides_.empty()) {
        return;
    }
    config_dir_ = cfg;
    // absolute: NapCat reads the file:// pictures from its own working directory (the bot's is "./resource")
    resource_dir_ = res_abs;
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
        const Json::Value &maps = J["maps"].isArray() ? J["maps"] : Json::Value(Json::arrayValue);
        for (const auto &m : maps) {
            s.maps.push_back({(e.path() / m["file"].asString()).string(), m["scale"].asDouble(), m["ox"].asDouble(),
                              m["oy"].asDouble()});
        }
        if (s.maps.empty()) { // data built before sections: one map, rooms numbered along the side
            const Json::Value &o = J["overview"];
            s.maps.push_back({(e.path() / o["file"].asString()).string(), o["scale"].asDouble(), o["ox"].asDouble(),
                              o["oy"].asDouble()});
        }
        for (const auto &r : J["rooms"]) {
            room_t rm{r["num"].asInt(), r["name"].asString(), r["x"].asDouble(), r["y"].asDouble(),
                      r["w"].asDouble(), r["h"].asDouble(), r.get("part", 1).asInt(), r.get("local", 0).asInt()};
            if (rm.local == 0) {
                rm.local = rm.num;
            }
            s.rooms.push_back(rm);
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

std::string celesteguessr::keys_text() const
{
    std::string keys;
    for (const auto &kv : sides_) {
        keys += (keys.empty() ? "" : " ") + kv.first;
    }
    return keys;
}

// ---- parsing -----------------------------------------------------------------------------------------------------

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

// "7A-3" -> 7A, section 3; "7A" -> 7A, section 0 (not given). False if not a place.
bool celesteguessr::parse_place(const std::string &tok, std::string &side, int &part) const
{
    const size_t dash = tok.rfind('-');
    part = 0;
    if (dash != std::string::npos && dash > 0) {
        const std::string p = tok.substr(dash + 1);
        if (!all_digits(p)) {
            return false;
        }
        side = side_key(tok.substr(0, dash));
        part = std::stoi(p);
    }
    else {
        side = side_key(tok);
    }
    return !side.empty();
}

// ---- scoring -----------------------------------------------------------------------------------------------------

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
// (x, y): the centre of the piece shown, world px.
int celesteguessr::score_for(const side_t &answer_side, int answer_room, double x, double y, const std::string &side,
                             const room_t *room)
{
    if (side != answer_side.key) {
        return chapter_of(side) == chapter_of(answer_side.key) ? 100 : 0;
    }
    if (room == nullptr) {
        return 0;
    }
    if (room->num == answer_room) {
        return 5000;
    }
    const double dx = std::max({room->x - x, 0.0, x - (room->x + room->w)});
    const double dy = std::max({room->y - y, 0.0, y - (room->y + room->h)});
    return static_cast<int>(std::lround(300.0 + 3700.0 * std::exp(-std::hypot(dx, dy) / 640.0)));
}

int celesteguessr::difficulty(const std::string &w)
{
    static const std::map<std::string, int> words = {
        {"easy", 1}, {"简单", 1}, {"hard", 2}, {"困难", 2}, {"ultra", 3}, {"极难", 3},
        {"imp", 4}, {"impossible", 4}, {"地狱", 4},
    };
    auto it = words.find(lower_ascii(w));
    return it == words.end() ? 0 : it->second;
}

std::string celesteguessr::difficulty_name(int zoom)
{
    switch (zoom) {
    case 2: return "困难（1/2 屏）";
    case 3: return "极难（1/3 屏）";
    case 4: return "地狱（1/4 屏）";
    default: return "简单（整屏）";
    }
}

// The question picture: the whole screenshot (easy), or a random 1/zoom of it scaled back up with nearest-neighbour;
// crops that are almost flat (sky, darkness, a plain wall) are re-rolled, as guessmap does.
void celesteguessr::make_question(const std::string &sc, round_t &r) const
{
    const shot_t &shot = sides_.at(r.side).shots[r.shot];
    r.cx = shot.x;
    r.cy = shot.y;
    r.question = shot.file;
    if (r.zoom <= 1) {
        return;
    }
    Magick::Image src(shot.file);
    const int W = static_cast<int>(src.columns()), H = static_cast<int>(src.rows());
    const int cw = W / r.zoom, ch = H / r.zoom;
    Magick::Image best;
    double best_sd = -1;
    int bx = 0, by = 0;
    for (int tries = 0; tries < 12; ++tries) {
        const int x0 = get_random(W - cw + 1), y0 = get_random(H - ch + 1);
        Magick::Image c = src;
        c.crop(Magick::Geometry(cw, ch, x0, y0));
        c.repage();
        Magick::Image probe = c;
        probe.sample(Magick::Geometry(32, 18));
        probe.type(Magick::GrayscaleType);
        double sum = 0, sum2 = 0;
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 32; ++x) {
                const double v = Magick::ColorGray(probe.pixelColor(x, y)).shade();
                sum += v;
                sum2 += v * v;
            }
        }
        const double mean = sum / 576, sd = std::sqrt(std::max(0.0, sum2 / 576 - mean * mean));
        if (sd > best_sd) {
            best_sd = sd;
            best = c;
            bx = x0;
            by = y0;
        }
        if (sd > 0.08) {
            break;
        }
    }
    best.filterType(Magick::PointFilter);
    best.sample(Magick::Geometry(static_cast<size_t>(W), static_cast<size_t>(H)));
    const fs::path dir = fs::path(resource_dir_) / "celesteguessr_tmp";
    fs::create_directories(dir);
    r.question = (dir / (sc + "_question.jpg")).string();
    best.quality(90);
    best.write(r.question);
    // the picture is 3x the game's 320x180 view, centred on the shot's camera
    r.cx = shot.x - 160 + (bx + cw / 2.0) * 320.0 / W;
    r.cy = shot.y - 90 + (by + ch / 2.0) * 180.0 / H;
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

// ---- sending -----------------------------------------------------------------------------------------------------

void celesteguessr::reply(const msg_meta &conf, const std::string &s) const
{
    conf.p->cq_send((conf.message_id ? "[CQ:reply,id=" + std::to_string(conf.message_id) + "]" : std::string()) + s,
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
        reply(conf, fallback);
    }
}

// Every section of a side: one picture, or a folded forward message with a node per section.
void celesteguessr::send_side_maps(const msg_meta &conf, const side_t &s, round_t *r)
{
    const std::string head = s.key + " " + s.name_zh + "（" + std::to_string(s.rooms.size()) + " 个房间";
    if (s.maps.size() == 1) {
        if (r != nullptr && !r->maps_sent.insert(s.key).second) {
            reply(conf, "已选 " + s.key + "，地图在上面，*cg <房间号> 作答");
            return;
        }
        reply(conf, head + "）*cg <房间号> 作答" + image(s.maps[0].file));
        return;
    }
    if (r != nullptr && !r->maps_sent.insert(s.key).second) {
        reply(conf, "已选 " + s.key + "，各段地图在上面：*cg <段> 看单段，或 *cg " + s.key + "-<段> <房间> 作答");
        return;
    }
    const std::string intro = head + "，分 " + std::to_string(s.maps.size()) + " 段）\n*cg <段> 只看一段，*cg " + s.key +
                              "-<段> <房间> 作答，如 *cg " + s.key + "-2 5";
    Json::Value messages(Json::arrayValue);
    const std::string uin = std::to_string(conf.p->get_botqq());
    auto node = [&](const std::string &content) {
        Json::Value n(Json::objectValue), d(Json::objectValue);
        n["type"] = "node";
        d["name"] = "CelesteGuessr";
        d["uin"] = uin;
        d["content"] = string_to_messageArr(content);
        n["data"] = d;
        messages.append(n);
    };
    node(intro);
    for (size_t i = 0; i < s.maps.size(); ++i) {
        const int part = static_cast<int>(i) + 1;
        node(s.key + "-" + std::to_string(part) + "（" + std::to_string(s.part_size(part)) + " 个房间）" +
             image(s.maps[i].file));
    }
    Json::Value req(Json::objectValue);
    std::string endpoint;
    if (conf.message_type == "group") {
        req["group_id"] = Json::UInt64(conf.group_id);
        endpoint = "send_group_forward_msg";
    }
    else {
        req["user_id"] = Json::UInt64(conf.user_id);
        endpoint = "send_private_forward_msg";
    }
    req["messages"] = messages;
    const Json::Value res = string_to_json(conf.p->cq_send(endpoint, req));
    if (!res.isObject() || res.get("status", "ok").asString() == "failed") { // plain message instead
        std::string all = intro;
        for (const auto &m : s.maps) {
            all += image(m.file);
        }
        reply(conf, all);
    }
}

void celesteguessr::send_part_map(const msg_meta &conf, const side_t &s, int part, round_t *r)
{
    const std::string tag = s.key + "-" + std::to_string(part);
    if (r != nullptr && !r->maps_sent.insert(tag).second) {
        reply(conf, "已选 " + tag + "，地图在上面，*cg <房间号> 作答");
        return;
    }
    reply(conf, tag + " " + s.name_zh + "（第 " + std::to_string(part) + "/" + std::to_string(s.maps.size()) + " 段，" +
                    std::to_string(s.part_size(part)) + " 个房间）*cg <房间号> 作答" +
                    image(s.maps[static_cast<size_t>(part) - 1].file));
}

// ---- a round -----------------------------------------------------------------------------------------------------

void celesteguessr::start_round(const msg_meta &conf, int seconds, int zoom)
{
    const std::string sc = scope(conf);
    auto &r = rounds_[sc];
    if (r.active) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(r.deadline - clock_t_::now()).count();
        conf.p->cq_send(image(r.question) + "本轮还在进行，剩 " +
                            std::to_string(std::max<long long>(0, left)) + " 秒。*cg <章节> 先选章节",
                        conf);
        return;
    }
    if (pool_.empty()) {
        conf.p->cq_send("CelesteGuessr 题库是空的（" + data_dir_ + "）", conf);
        return;
    }
    auto &recent = recent_[sc];
    std::pair<std::string, size_t> pk;
    for (int tries = 0; tries < 50; ++tries) {
        pk = pool_[get_random(static_cast<int>(pool_.size()))];
        if (std::find(recent.begin(), recent.end(), pk) == recent.end()) {
            break;
        }
    }
    recent.push_back(pk);
    while (recent.size() > std::min(kRecent, pool_.size() / 2)) {
        recent.pop_front();
    }
    r = round_t{};
    r.active = true;
    r.side = pk.first;
    r.shot = pk.second;
    r.seconds = seconds;
    r.started = clock_t_::now();
    r.deadline = r.started + std::chrono::seconds(seconds);
    r.starter = conf.user_id;
    r.message_type = conf.message_type;
    r.user_id = conf.user_id;
    r.group_id = conf.group_id;
    r.last_activity = r.started;
    r.zoom = std::clamp(zoom, 1, 4);
    try {
        make_question(sc, r);
    }
    catch (const std::exception &e) { // the whole screenshot then
        set_global_log(LOG::ERROR, std::string("celesteguessr: crop: ") + e.what());
        r.zoom = 1;
        r.question = sides_.at(r.side).shots[r.shot].file;
        r.cx = sides_.at(r.side).shots[r.shot].x;
        r.cy = sides_.at(r.side).shots[r.shot].y;
    }
    conf.p->cq_send(image(r.question) +
                        "CelesteGuessr · " + difficulty_name(r.zoom) + "：这张截图在哪里？\n"
                        "① *cg 1A 先选章节（会发这一章的地图；大章节按存档点分段，如 7A-3）\n"
                        "② *cg 19 按地图上的号码猜房间（每人一次）\n"
                        "一步到位：*cg 1A 19 · *cg 7A-3 5 · *cg check 看进度\n"
                        "限时 " + std::to_string(seconds) + " 秒 · 难度：*cg easy/hard/ultra/imp",
                    conf);
}

void celesteguessr::pick(const msg_meta &conf, round_t &r, const std::string &side, int part)
{
    auto it = sides_.find(side);
    if (it == sides_.end()) {
        reply(conf, "题库里还没有 " + side + "（现有：" + keys_text() + "）");
        return;
    }
    const side_t &s = it->second;
    if (part < 0 || part > static_cast<int>(s.maps.size())) {
        reply(conf, s.key + " 只有 " + std::to_string(s.maps.size()) + " 段");
        return;
    }
    if (s.maps.size() == 1) {
        part = 1;
    }
    r.picks[conf.user_id] = {s.key, part};
    r.last_activity = clock_t_::now();
    if (part == 0) {
        send_side_maps(conf, s, &r);
    }
    else if (s.maps.size() == 1) {
        send_side_maps(conf, s, &r);
    }
    else {
        send_part_map(conf, s, part, &r);
    }
}

void celesteguessr::record_guess(const msg_meta &conf, round_t &r, const std::string &side, int room)
{
    const side_t &ans = sides_.at(r.side);
    const side_t &gs = sides_.at(side);
    guess_t g;
    g.user = conf.user_id;
    g.name = display_name_in_group(conf, conf.user_id);
    g.side = side;
    g.room = room;
    g.score = score_for(ans, ans.shots[r.shot].room, r.cx, r.cy, side, gs.room(room));
    g.secs = std::chrono::duration<double>(clock_t_::now() - r.started).count();
    r.guesses.push_back(g);
    r.picks[conf.user_id] = {side, gs.room(room) ? gs.room(room)->part : 1};
    r.last_activity = clock_t_::now();
    react(conf, "9989", "收到：" + gs.label(room));
}

// What a player typed after *cg, in an open round: a chapter, a section, a room, or several at once.
void celesteguessr::handle_answer(const msg_meta &conf, round_t &r, const std::vector<std::string> &t)
{
    auto pk = r.picks.find(conf.user_id);
    const side_t *ps = pk != r.picks.end() && sides_.count(pk->second.side) ? &sides_.at(pk->second.side) : nullptr;
    const int ppart = ps ? pk->second.part : 0;

    // the room in a known side and section: number (within the section) or room name
    auto guess_room = [&](const side_t &s, int part, const std::string &tok) {
        const room_t *rm = all_digits(tok) ? s.room_at(part, std::stoi(tok)) : s.room_named(tok);
        if (rm == nullptr) {
            const std::string where = s.maps.size() > 1 ? s.key + "-" + std::to_string(part) : s.key;
            reply(conf, where + " 没有房间 " + tok + "（1-" + std::to_string(s.part_size(part)) + "）");
            return;
        }
        record_guess(conf, r, s.key, rm->num);
    };
    auto side_or_err = [&](const std::string &key) -> const side_t * {
        auto it = sides_.find(key);
        if (it == sides_.end()) {
            reply(conf, "题库里还没有 " + key + "（现有：" + keys_text() + "）");
            return nullptr;
        }
        return &it->second;
    };

    if (t.size() == 1) {
        const std::string &tok = t[0];
        if (ps != nullptr && all_digits(tok)) { // after a pick a bare number is a section, then a room
            if (ps->maps.size() > 1 && ppart == 0) {
                pick(conf, r, ps->key, std::stoi(tok));
            }
            else {
                guess_room(*ps, std::max(ppart, 1), tok);
            }
            return;
        }
        std::string side;
        int part = 0;
        if (parse_place(tok, side, part)) { // "7A", "7A-3", "1" (= 1A when nothing is picked)
            auto it = sides_.find(side);
            if (part > 0 && it != sides_.end() && it->second.maps.size() == 1) { // "1A-19" on a one-section side
                guess_room(it->second, 1, std::to_string(part));
                return;
            }
            pick(conf, r, side, part);
            return;
        }
        if (ps != nullptr && ppart > 0) { // a room name, e.g. "8zb"
            guess_room(*ps, ppart, tok);
            return;
        }
        reply(conf, all_digits(tok) ? "先选章节：*cg 1A，再 *cg " + tok : "格式：*cg 1A 19 · *cg 7A-3 5");
        return;
    }
    // several at once: "1A 19", "7A-3 5", "7A 3 5"; "7A 3" on a big side picks section 3
    std::string side;
    int part = 0;
    if (!parse_place(t[0], side, part)) {
        reply(conf, "格式：*cg 1A 19 · *cg 7A-3 5");
        return;
    }
    const side_t *s = side_or_err(side);
    if (s == nullptr) {
        return;
    }
    size_t i = 1;
    if (part == 0 && s->maps.size() > 1) {
        if (!all_digits(t[i])) {
            reply(conf, s->key + " 分 " + std::to_string(s->maps.size()) + " 段：*cg " + s->key + "-<段> <房间>");
            return;
        }
        part = std::stoi(t[i++]);
        if (i >= t.size()) {
            pick(conf, r, s->key, part);
            return;
        }
    }
    if (s->maps.size() == 1) {
        part = 1;
    }
    if (part < 1 || part > static_cast<int>(s->maps.size())) {
        reply(conf, s->key + " 只有 " + std::to_string(s->maps.size()) + " 段");
        return;
    }
    guess_room(*s, part, t[i]);
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
    conf.p->cq_send("CelesteGuessr 进行中（" + difficulty_name(r.zoom) + "），剩 " +
                        std::to_string(std::max<long long>(0, left)) + " 秒\n已作答（" +
                        std::to_string(r.guesses.size()) + "）：" + (done.empty() ? "无" : done) +
                        (waiting.empty() ? "" : "\n选了章节还没猜：" + waiting) +
                        "\n所有选了章节的人都猜完后 10 秒自动揭晓",
                    conf);
}

std::string celesteguessr::draw_reveal(const std::string &sc, const round_t &r) const
{
    const side_t &s = sides_.at(r.side);
    const shot_t &shot = s.shots[r.shot];
    const room_t *ans = s.room(shot.room);
    const int part = ans ? ans->part : 1;
    const map_t &m = s.maps[static_cast<size_t>(part) - 1];
    Magick::Image img(m.file);
    auto X = [&](double wx) { return (wx - m.ox) * m.scale; };
    auto Y = [&](double wy) { return (wy - m.oy) * m.scale; };

    // the players' rooms on this section, numbered by their place in the results
    std::vector<guess_t> order = r.guesses;
    std::stable_sort(order.begin(), order.end(), [](const guess_t &a, const guess_t &b) {
        return a.score != b.score ? a.score > b.score : a.secs < b.secs;
    });
    for (size_t i = 0; i < order.size(); ++i) {
        const room_t *rm = order[i].side == s.key ? s.room(order[i].room) : nullptr;
        if (rm == nullptr || rm->part != part) {
            continue;
        }
        img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor("none"), Magick::DrawableStrokeColor("#00c8ff"),
                                               Magick::DrawableStrokeWidth(4),
                                               Magick::DrawableRectangle(X(rm->x), Y(rm->y), X(rm->x + rm->w),
                                                                         Y(rm->y + rm->h))});
        // top-right corner: the room's own number badge is top-left
        badge(img, X(rm->x + rm->w) - 18, Y(rm->y) + 18, 14, Magick::Color("#0090c0"), std::to_string(i + 1));
    }
    // the answer: the piece shown and its centre (a frame only: the room's shape stays visible)
    const double cx = X(r.cx), cy = Y(r.cy), hw = std::max(160.0 / r.zoom * m.scale, 6.0),
                 hh = std::max(90.0 / r.zoom * m.scale, 4.0);
    (void)shot;
    img.draw(std::vector<Magick::Drawable>{Magick::DrawableFillColor("none"), Magick::DrawableStrokeColor("#ffd600"),
                                           Magick::DrawableStrokeWidth(5),
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
    std::string msg = why + "！答案：" + s.name_zh + " · " + s.label(shot.room) + " 号房间" +
                      (ans ? "（" + ans->name + "）" : "") + " · " + difficulty_name(r.zoom);
    if (order.empty()) {
        msg += "\n没有人作答。";
    }
    for (size_t i = 0; i < order.size(); ++i) {
        const auto &g = order[i];
        const auto gs = sides_.find(g.side);
        msg += "\n" + std::to_string(i + 1) + ". " + g.name + "  " + std::to_string(g.score) + " 分 " + tier(g.score) +
               "（" + (gs != sides_.end() ? gs->second.label(g.room) : g.side) + "，" + fmt_secs(g.secs) + " 秒）";
    }
    std::string img;
    try {
        img = draw_reveal(sc, r);
    }
    catch (const std::exception &e) {
        set_global_log(LOG::ERROR, std::string("celesteguessr: reveal: ") + e.what());
    }
    p->cq_send(msg + (img.empty() ? "" : image(img)), conf);
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
    std::string side;
    int part = 0;
    if (!parse_place(arg, side, part)) {
        conf.p->cq_send("*cg map <章节>，如 *cg map 1A、*cg map 7A-3；现有：" + keys_text(), conf);
        return;
    }
    auto it = sides_.find(side);
    if (it == sides_.end()) {
        conf.p->cq_send("题库里还没有 " + side, conf);
        return;
    }
    if (part > static_cast<int>(it->second.maps.size())) {
        conf.p->cq_send(side + " 只有 " + std::to_string(it->second.maps.size()) + " 段", conf);
        return;
    }
    if (part == 0 || it->second.maps.size() == 1) {
        send_side_maps(conf, it->second, nullptr);
    }
    else {
        send_part_map(conf, it->second, part, nullptr);
    }
}

static std::string help_text()
{
    return "CelesteGuessr：一张截图，猜它在蔚蓝的哪个房间\n"
           "*cg：开始一轮（*cg 开始 60 = 限时 60 秒）\n"
           "难度：*cg easy 整屏 · hard 1/2 屏 · ultra 1/3 屏 · imp 1/4 屏（简单/困难/极难/地狱，可加秒数：*cg hard 60）\n"
           "*cg 1A：选章节，发这一章的地图（选完还能换）；大章节按存档点分段，会把各段折叠发出\n"
           "*cg 7A-3 或选完章节后 *cg 3：只看第 3 段\n"
           "*cg 19：按地图上的号码猜房间（每人一次）；一步到位：*cg 1A 19 · *cg 7A-3 5\n"
           "*cg check：看谁猜了、还剩多久\n"
           "*cg map 1A / 7A-3：随时看地图 · *cg 结束：提前揭晓（发起人或管理）· *cg 排行：本群总分\n"
           "计分：正中 5000；同一面越远越少（隔壁约 3500，两屏外约 1650，最少 300）；同章节另一面 100；别的章节 0。"
           "时间到、或选了章节的人都猜完 10 秒后揭晓。\n"
           "章节：0 序幕，1A-7C，E 尾声，8A-8C 核心，9 再见（选了章节后纯数字是段号/房间号；换到再见用 *cg fw）";
}

// ---- commands ----------------------------------------------------------------------------------------------------

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
    for (const char *sep : {"#", "/", ",", "，", "号", "房间", "　"}) {
        body = replace_all(body, sep, " ");
    }
    const std::vector<std::string> t = split_ws(body);
    const std::string cmd = t.empty() ? "" : lower_ascii(t[0]);
    const std::string arg = t.size() > 1 ? t[1] : "";

    // start: "*cg", "*cg 开始 60", "*cg hard", "*cg 开始 ultra 90" (seconds and difficulty in any order)
    if (cmd.empty() || cmd == "start" || cmd == "开始" || cmd == "s" || difficulty(cmd) > 0) {
        int secs = kDefaultSeconds, zoom = 1;
        for (size_t i = 0; i < t.size(); ++i) {
            if (all_digits(t[i])) {
                secs = std::clamp(std::stoi(t[i]), kMinSeconds, kMaxSeconds);
            }
            else if (difficulty(t[i]) > 0) {
                zoom = difficulty(t[i]);
            }
        }
        start_round(conf, secs, zoom);
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

    // a chapter, a section or a guess
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
    handle_answer(conf, r, t);
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
                finish_round(kv.first, r, p, all_done && now < r.deadline ? "大家都猜完了" : "时间到");
            }
            catch (const std::exception &e) {
                r.active = false;
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
    rounds_.clear(); // rounds index the sides being reloaded
    sync_dirs(conf.p);
    return !pool_.empty();
}

std::string celesteguessr::help()
{
    return "CelesteGuessr：根据截图猜蔚蓝的房间。帮助：*cg help";
}

DECLARE_FACTORY_FUNCTIONS(celesteguessr)
