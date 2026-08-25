#pragma once

#include "processable.h"

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// dateimg — daily 00:00 date-sign image pusher.
//
// Each day at local 00:00 it renders today's date onto a chosen background
// (a character holding a blank sign) and pushes it to every ENABLED group
// (default disabled), one RANDOM background per group. Above the Gregorian
// date it draws the lunar day-of-month in smaller red 大写 numerals. Signs that
// are square/circular use a two-line layout (x月 / x日); rectangles use one
// line.
//
// Config + the background set are re-read from disk on every render, so edits
// (including the bot-admin upload/edit chat commands) take effect with no
// reload command. An already-generated output for today is reused by the daily
// push.
//
// Sources are split across: dateimg.cpp (hooks/dispatch/daily/render glue),
// dateimg_config.cpp (config + background scan), dateimg_commands.cpp (cmd_*),
// render.{h,cpp} (image engine), lunar.h (lunar day), dateimg_util.h (helpers).
class dateimg : public processable {
private:
    // Per-background text placement. (x,y) is the CENTER of the whole text
    // block (lunar line + date), so it stays put for any date length.
    struct placement {
        double x = 0;
        double y = 0;
        double font_size = 48;
        double rotation = 0; // degrees clockwise, to match the sign tilt
        std::string color = "black";
        std::string font;              // font file in fonts/; "" = default
        std::string layout = "single"; // "single" | "double" (two-line)
    };

    mutable std::mutex mu_;

    // group_id -> chosen background ("" = random each day). Presence ==
    // enabled.
    std::map<groupid_t, std::string> subs_;

    std::map<std::string, placement> placements_; // bg filename -> placement
    std::string default_font_;
    std::string mode_ = "gif";  // "gif" (sticker) | "png" (image)
    bool lunar_enabled_ = true; // draw the lunar date above the Gregorian date
    double lunar_scale_ = 0.6;  // lunar line size relative to the date font
    int output_max_px_ = 1280;
    std::string last_fired_ymd_; // persisted 00:00 latch (survives reload)

    // bot-admin upload: user_id -> pending background key awaiting an image
    std::map<userid_t, std::string> pending_upload_;

    // daily blessing line sent with the image (messages.json)
    bool msg_enabled_ = true;
    std::string msg_prefix_ = "猫好~，今天是"; // line before the image
    std::string msg_lead_;                     // prefix before the sentence
    std::vector<std::string> sentences_;       // random pool

    // per-group birthday list (birthdays.json). A birthday fires when today's
    // SOLAR date (solar entry) or today's LUNAR date (lunar entry) matches.
    struct bday_entry {
        std::string who; // full line to send, e.g. "祝失序生日快乐！"
        bool lunar = false;
        int mm = 0; // month (solar) or lunar-month
        int dd = 0; // day   (solar) or lunar-day
    };
    std::map<groupid_t, std::vector<bday_entry>> birthdays_;

    std::string subs_path_;
    std::string bgcfg_path_;
    std::string msgs_path_;  // config/features/dateimg/messages.json
    std::string bdays_path_; // config/features/dateimg/birthdays.json
    std::string bg_dir_;
    std::string font_dir_;
    std::string out_dir_;

    // --- config / scan (dateimg_config.cpp) ---
    void load_subs_unlocked();
    void save_subs_unlocked() const;
    void load_bgcfg_unlocked();
    void load_messages_unlocked();  // messages.json -> prefix/lead/sentences
    void load_birthdays_unlocked(); // birthdays.json -> per-group lists
    void save_birthdays_unlocked() const;
    // full wish lines for group g whose date matches today (solar+lunar)
    std::vector<std::string> birthday_wishes_unlocked(groupid_t g,
                                                      const std::tm &lt) const;
    std::vector<std::string> available_backgrounds() const;
    std::string pick_background_unlocked(const std::string &pinned) const;
    std::string resolve_font_unlocked(const placement &pl) const;
    void invalidate_cache_unlocked(const std::string &bg_key);
    // resolve an admin arg (numeric index into the sorted list, or a name/stem)
    // to an actual background filename; "" if not found.
    std::string resolve_bg(const std::string &arg) const;

    // --- rendering / sending (dateimg.cpp) ---
    // Render today's date onto bg_name; returns abs output path or "".
    // force=true ignores (and refreshes) the cached output.
    std::string render_image(const std::string &bg_name, bool force);
    void send_forward_images(const std::vector<std::string> &abs_paths,
                             const msg_meta &conf);
    // build the daily message: prefix line + image + (lead + random sentence)
    // + birthday wishes for group_id (0 = none). Reads config fresh; fresh
    // random sentence each call.
    std::string compose_daily_message(const std::string &img_abs,
                                      groupid_t group_id);
    static std::string today_date_text();
    void daily_task(bot *p);
    void push_all(bot *p);

    // --- commands (dateimg_commands.cpp) ---
    void cmd_on(const msg_meta &conf);
    void cmd_off(const msg_meta &conf);
    void cmd_bg(const std::string &arg, const msg_meta &conf);
    void cmd_list(const msg_meta &conf);
    void cmd_test(const std::string &arg, const msg_meta &conf); // one / all
    void cmd_addbg(const std::string &arg, const std::string &message,
                   const msg_meta &conf);
    void cmd_setcfg(const std::string &arg, const msg_meta &conf);
    void cmd_delbg(const std::string &arg, const msg_meta &conf);
    void cmd_rename(const std::string &arg, const msg_meta &conf);
    void cmd_mode(const std::string &arg, const msg_meta &conf);
    void cmd_msg(const std::string &arg, const msg_meta &conf); // sentence mgmt
    void cmd_date(const std::string &raw,
                  const msg_meta &conf); // date.* birthday
    void store_bg_from_url(const std::string &url, const std::string &key,
                           const msg_meta &conf);
    // render bg fresh and send it to conf as a preview (used by setcfg/addbg)
    void send_preview(const std::string &bg_name, const msg_meta &conf);
    static std::string detailed_help(help_level_t level);

public:
    dateimg();

    void process(std::string message, const msg_meta &conf) override;
    bool check(std::string message, const msg_meta &conf) override;
    std::string help() override;
    bool reload(const msg_meta &conf) override;
    void
    set_callback(std::function<void(std::function<void(bot *p)>)> f) override;
    void set_backup_files(archivist *p, const std::string &name) override;
};

DECLARE_FACTORY_FUNCTIONS_HEADER
