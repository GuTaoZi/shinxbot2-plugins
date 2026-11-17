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
// guesses the room (or *cg 1A 19 at once). Big chapters are split at checkpoints into sections, numbered within each
// ("maybe use things like 7A-3 5? and send forward folded messages after chosen a chapter, with each subchapter a
// image, and if chosen the subchapter initially or not deciding the room, then show just the single subchapter"):
// *cg 7A posts every section in one folded forward message, *cg 7A-3 (or *cg 3 after *cg 7A) posts that section
// alone, *cg 5 then guesses its room 5 (or *cg 7A-3 5 at once).
// Scoring ("the farther, the lower score ... shrink like exponential, maybe bullseye 3, then close 2 and same chapter
// 1"): the right room 5000 (正中), another room of the right side 300 + 3700·e^(-d/640 px) by its distance to the
// spot (很近 at 1500+, else 同一面), the right chapter's other side 100, another chapter 0. A round ends when its
// time is up, early once everyone who picked a chapter has guessed and 10 s passed quietly, or by *cg 结束;
// *cg check shows who has guessed. In groups players are named by their group card.
// Difficulty (user: "add difficulty just like map guess? change the screenshot's size"; guessmap crops 256/128/64):
// *cg easy|hard|ultra|imp (简单/困难/极难/地狱) shows the whole screen, or a random 1/2, 1/3, 1/4 of it scaled back up
// with nearest-neighbour; flat crops are re-rolled; distance is measured from the crop's centre.
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
        int num = 0; // along the whole side
        std::string name;
        double x = 0, y = 0, w = 0, h = 0;
        int part = 1, local = 0; // section, number within it
    };
    struct map_t {
        std::string file;
        double scale = 1, ox = 0, oy = 0; // picture px = (world - o) * scale
    };
    struct shot_t {
        std::string file;
        int room = 0;
        double x = 0, y = 0; // camera centre, world px
    };
    struct side_t {
        std::string key, name_zh, name_en, dir;
        std::vector<map_t> maps; // one per section
        std::vector<room_t> rooms;
        std::vector<shot_t> shots;
        const room_t *room(int num) const;
        const room_t *room_at(int part, int local) const;
        const room_t *room_named(const std::string &name) const;
        int part_size(int part) const;
        std::string label(int num) const; // "7A-3 5" or "1A 19"
    };
    struct pick_t {
        std::string side;
        int part = 0; // 0 = section not chosen yet (big sides)
    };
    struct guess_t {
        userid_t user = 0;
        std::string name, side;
        int room = 0; // num
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
        std::map<userid_t, pick_t> picks;
        std::set<std::string> maps_sent; // "7A" (every section) or "7A-3"
        std::chrono::steady_clock::time_point last_activity;
        int zoom = 1;             // 1 easy (whole screen), 2 hard, 3 ultra, 4 imp: the piece is 1/zoom of the screen
        double cx = 0, cy = 0;    // centre of the piece shown, world px
        std::string question;     // the picture sent
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
    std::string keys_text() const;

    static std::string side_key(std::string s);
    bool parse_place(const std::string &tok, std::string &side, int &part) const;
    static int score_for(const side_t &answer_side, int answer_room, double x, double y, const std::string &side,
                         const room_t *room);
    static int difficulty(const std::string &word); // 0 if not a difficulty word
    static std::string difficulty_name(int zoom);
    void make_question(const std::string &sc, round_t &r) const;
    static std::string tier(int score);
    static std::string chapter_of(const std::string &side_key);

    void start_round(const msg_meta &conf, int seconds, int zoom);
    void send_side_maps(const msg_meta &conf, const side_t &s, round_t *r);
    void send_part_map(const msg_meta &conf, const side_t &s, int part, round_t *r);
    void pick(const msg_meta &conf, round_t &r, const std::string &side, int part);
    void record_guess(const msg_meta &conf, round_t &r, const std::string &side, int room);
    void handle_answer(const msg_meta &conf, round_t &r, const std::vector<std::string> &t);
    void send_check(const msg_meta &conf);
    void finish_round(const std::string &sc, round_t &r, bot *p, const std::string &why);
    std::string draw_reveal(const std::string &sc, const round_t &r) const;
    void save_scores(const std::string &sc, const round_t &r);
    void send_rank(const msg_meta &conf);
    void send_map(const msg_meta &conf, const std::string &arg);
    void reply(const msg_meta &conf, const std::string &text) const;
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
