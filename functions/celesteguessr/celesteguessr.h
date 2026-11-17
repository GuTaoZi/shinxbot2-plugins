#pragma once

// CelesteGuessr for QQ: one screenshot from anywhere in Celeste, guess the side and the room.
//
// History of user requests (2026-11-17, GuTao): "understand the rule of celesteguessr.com, and using Celeste TAS or
// ScreenshotTool ... see if we can build one game like this implemented with the bot as a plugin"; after a 1A pilot,
// "Write the plugin first". CelesteGuessr shows one screenshot; players pick the chapter and side, pin the spot on a
// map of tile silhouettes, and score up to 5000 by distance (time breaks ties; its public settings cap the right
// chapter's wrong side at 100). QQ cannot click a map, so (user: "first guess the chapter, then provide a map, then
// number"): *cg 1A picks the chapter and the bot posts that chapter's map once per round - drawn like the F6 debug
// map, each room's number in a corner badge ("most of us just remember the shape of landscape") - then *cg 19
// guesses the room (or *cg 1A 19 at once). Scoring ("the farther, the lower score ... shrink like exponential, maybe
// bullseye 3, then close 2 and same chapter 1"): the right room 5000 (正中), another room of the right side
// 300 + 3700·e^(-d/640 px) by its distance to the spot (很近 at 1500+, else 同一面), the right chapter's other
// side 100, another chapter 0. A round ends when its time is up, early once everyone who picked a chapter has guessed and 10 s
// passed quietly, or by *cg 结束; *cg check shows who has guessed. In groups players are named by their group card.
// Data (screenshots, rooms, maps) is built on the PC with the game by tools/cg_build_data.py into
// resource/celesteguessr/<side>/.

#include "processable.h"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class celesteguessr : public processable {
public:
    struct room_t {
        int num = 0;
        std::string name;
        double x = 0, y = 0, w = 0, h = 0;
    };
    struct shot_t {
        std::string file;
        int room = 0;
        double x = 0, y = 0; // camera centre, world px
    };
    struct side_t {
        std::string key, name_zh, name_en, dir, overview;
        double scale = 1, ox = 0, oy = 0; // overview px = (world - o) * scale
        std::vector<room_t> rooms;
        std::vector<shot_t> shots;
        const room_t *room(int num) const;
        const room_t *room_named(const std::string &name) const;
    };
    struct guess_t {
        userid_t user = 0;
        std::string name, side;
        int room = 0;
        int score = 0;
        double secs = 0;
    };
    struct round_t {
        bool active = false;
        std::string side;
        size_t shot = 0;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point deadline;
        int seconds = 0;
        userid_t starter = 0;
        std::string message_type; // where to answer (msg_meta has no copy assignment)
        userid_t user_id = 0;
        groupid_t group_id = 0;
        std::vector<guess_t> guesses;
        std::map<userid_t, std::string> picks;   // chapter each player picked
        std::set<std::string> maps_sent;         // chapter maps already posted this round
        std::chrono::steady_clock::time_point last_activity;
    };

private:
    std::mutex mu_;
    std::string data_dir_, config_dir_, resource_dir_;
    std::map<std::string, side_t> sides_; // key -> side
    std::vector<std::pair<std::string, size_t>> pool_; // every (side, shot)
    std::map<std::string, round_t> rounds_;            // scope -> round
    std::map<std::string, std::deque<std::pair<std::string, size_t>>> recent_;

    static constexpr int kDefaultSeconds = 120;
    static constexpr int kMinSeconds = 20;
    static constexpr int kMaxSeconds = 900; // CelesteGuessr's room limit: 15 min
    static constexpr size_t kRecent = 30;

    void sync_dirs(const bot *p);
    bool load_data();
    std::string scope(const msg_meta &conf) const;
    std::string scores_path() const;

    static std::string side_key(std::string s);
    bool parse_guess(const std::string &body, std::string &side, int &room, std::string &err) const;
    static int score_for(const side_t &answer_side, const shot_t &shot, const std::string &side,
                         const room_t *room);
    static std::string tier(int score);
    static std::string chapter_of(const std::string &side_key);
    void pick_chapter(const msg_meta &conf, round_t &r, const std::string &side);
    void record_guess(const msg_meta &conf, round_t &r, const std::string &side, int room);
    void send_check(const msg_meta &conf);

    void start_round(const msg_meta &conf, int seconds);
    void finish_round(const std::string &sc, round_t &r, bot *p, const std::string &why);
    std::string draw_reveal(const std::string &sc, const round_t &r) const;
    void save_scores(const std::string &sc, const round_t &r);
    void send_rank(const msg_meta &conf);
    void send_map(const msg_meta &conf, const std::string &arg);
    void react(const msg_meta &conf, const std::string &emoji, const std::string &fallback) const;
    void tick(bot *p);

public:
    celesteguessr();
    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    bool reload(const msg_meta &conf) override;
    void set_callback(std::function<void(std::function<void(bot *p)>)> f) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
