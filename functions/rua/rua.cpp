#include "rua.h"

#include "utils.h"

#include <Magick++.h>

#include <algorithm>
#include <cmath>
#include <list>
#include <sstream>

namespace {
constexpr const char *CMD_HELP = "*rua.help";
constexpr const char *CMD_RELOAD = "*rua.reload";
constexpr const char *CMD_FAVOR_EN = "*rua.favor";
constexpr const char *CMD_FAVOR_ZH = "*查询92好感度";
constexpr const char *CMD_LIST = "*rua.list";
constexpr const char *CMD_DRAW = "*rua";
constexpr const char *CMD_GET = "*rua.get ";
constexpr const char *CMD_ADD = "*rua.add ";
constexpr const char *CMD_DEL = "*rua.del ";

bool is_external_image_ref(const std::string &path)
{
    return cmd_match_prefix(path,
                            {"http://", "https://", "file://", "base64://"});
}

std::vector<std::string> split_by_char(const std::string &s, char delim)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == delim) {
            out.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur.push_back(ch);
        }
    }
    out.push_back(trim(cur));
    return out;
}

std::string pick_extension_from_url(const std::string &url)
{
    size_t qpos = url.find('?');
    std::string base = qpos == std::string::npos ? url : url.substr(0, qpos);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= base.size()) {
        return ".png";
    }
    std::string ext = base.substr(dot);
    if (ext.size() > 8 || ext.find('/') != std::string::npos) {
        return ".png";
    }
    return ext;
}

std::string extract_cq_param_from_message(const std::string &message,
                                          const std::string &key)
{
    const std::string token = "," + key + "=";
    size_t img_pos = message.find("[CQ:image");
    if (img_pos == std::string::npos) {
        return "";
    }

    size_t key_pos = message.find(token, img_pos);
    if (key_pos == std::string::npos) {
        return "";
    }

    const size_t start = key_pos + token.size();
    const size_t comma = message.find(',', start);
    const size_t bracket = message.find(']', start);
    const size_t end = std::min(comma == std::string::npos ? bracket : comma,
                                bracket == std::string::npos ? comma : bracket);
    if (end == std::string::npos || end <= start) {
        return "";
    }
    return message.substr(start, end - start);
}

std::string pick_extension_from_magick_format(std::string fmt)
{
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (fmt == "gif") {
        return ".gif";
    }
    if (fmt == "jpeg" || fmt == "jpg") {
        return ".jpg";
    }
    if (fmt == "png") {
        return ".png";
    }
    if (fmt == "webp") {
        return ".webp";
    }
    return "";
}

std::string today_date_string()
{
    std::time_t now = std::time(nullptr);
    std::tm local_tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

std::string rua_detail_help_public()
{
    return "*rua: 随机rua一只九二\n"
           "*查询92好感度: 查看九二好感\n"
           "*rua.help: 查看帮助";
}

std::string rua_detail_help(help_level_t level)
{
    std::string base = rua_detail_help_public();
    if (level == help_level_t::group_admin ||
        level == help_level_t::bot_admin) {
        return base + "\n"
                      "管理员命令:\n"
                      "*rua.reload\n"
                      "*rua.add 名称|描述|favor(可选) 然后发送图片\n"
                      "*rua.get 名称\n"
                      "*rua.del 名称\n"
                      "*rua.list";
    }
    return base;
}
} // namespace

rua::rua()
{
    fs::create_directories(rua_resource_dir());
    load_config();
    load_favor_text_config();
    load_favor_storage();
    load_daily_limit_storage();
}

std::string rua::rua_config_path() const
{
    return (fs::path(config_dir_) / "features/rua/rua.json").string();
}

bool rua::reload(const msg_meta &conf)
{
    std::lock_guard<std::mutex> guard(mutex_);
    sync_dirs_from_bot(conf.p);
    load_config();
    load_favor_text_config();
    load_favor_storage();
    load_daily_limit_storage();
    return true;
}

std::string rua::rua_state_path() const
{
    return (fs::path(config_dir_) / "features/rua/rua_state.json").string();
}

fs::path rua::rua_resource_dir() const
{
    return fs::path(resource_dir_) / "rua";
}

void rua::sync_dirs_from_bot(const bot *p)
{
    if (p == nullptr) {
        return;
    }
    const std::string new_cfg = p->getConfigDir();
    const std::string new_res = p->getResourceDir();
    if (new_cfg == config_dir_ && new_res == resource_dir_) {
        return;
    }
    config_dir_ = new_cfg;
    resource_dir_ = new_res;
    fs::create_directories(rua_resource_dir());
    load_config();
    load_favor_text_config();
    load_favor_storage();
    load_daily_limit_storage();
}

void rua::load_config()
{
    items_.clear();
    random_favor_mode_ = false;

    Json::Value root = string_to_json(readfile(rua_config_path(), "[]"));
    Json::Value arr = root;
    if (root.isObject() && root.isMember("items") && root["items"].isArray()) {
        arr = root["items"];
    }

    if (!arr.isArray()) {
        return;
    }

    for (const auto &item : arr) {
        if (!item.isObject()) {
            continue;
        }

        rua_item one;
        one.name = trim(item.get("name", "").asString());
        one.description = trim(item.get("description", "").asString());
        one.image = trim(item.get("image", "").asString());
        one.favor = item.isMember("favor") ? item["favor"].asInt()
                                           : item.get("val", 0).asInt();

        if (one.name.empty()) {
            continue;
        }

        items_.push_back(one);
    }

    if (!items_.empty()) {
        recompute_random_mode_unlocked();
    }
}

void rua::save_config_unlocked() const
{
    Json::Value arr(Json::arrayValue);
    for (const auto &it : items_) {
        Json::Value J(Json::objectValue);
        J["name"] = it.name;
        J["description"] = it.description;
        J["image"] = it.image;
        J["favor"] = it.favor;
        arr.append(J);
    }

    Json::Value root = string_to_json(readfile(rua_config_path(), "{}"));
    if (!root.isObject()) {
        root = Json::Value(Json::objectValue);
    }
    root["items"] = arr;
    writefile(rua_config_path(), root.toStyledString(), false);
}

void rua::load_favor_text_config()
{
    favor_text_levels_.clear();

    auto parse_levels = [&](const Json::Value &root) {
        Json::Value arr;
        if (root.isObject() && root.isMember("levels") &&
            root["levels"].isArray()) {
            arr = root["levels"];
        }
        if (!arr.isArray()) {
            return;
        }

        for (const auto &it : arr) {
            if (!it.isObject()) {
                continue;
            }

            favor_text_level one;
            one.min = it.get("min", 0).asInt();
            one.hearts = trim(it.get("hearts", "♡").asString());

            // New schema: one shared text pool for both query and draw.
            if (it.isMember("texts") && it["texts"].isArray()) {
                for (const auto &t : it["texts"]) {
                    std::string s = trim(t.asString());
                    if (!s.empty()) {
                        one.texts.push_back(s);
                    }
                }
            }

            // Backward compatibility with old query/draw schema.
            auto push_if = [&](const Json::Value &arrv) {
                if (!arrv.isArray()) {
                    return;
                }
                for (const auto &x : arrv) {
                    std::string s = trim(x.asString());
                    if (!s.empty()) {
                        one.texts.push_back(s);
                    }
                }
            };
            std::string query_text = trim(it.get("query", "").asString());
            std::string draw_text = trim(it.get("draw", "").asString());
            if (!query_text.empty()) {
                one.texts.push_back(query_text);
            }
            if (!draw_text.empty()) {
                one.texts.push_back(draw_text);
            }
            push_if(it["query_alt"]);
            push_if(it["draw_alt"]);

            if (one.texts.empty()) {
                continue;
            }

            if (one.hearts.empty()) {
                one.hearts = "♡";
            }
            favor_text_levels_.push_back(one);
        }
    };

    Json::Value root = string_to_json(readfile(rua_config_path(), "{}"));
    parse_levels(root);
    if (favor_text_levels_.empty()) {
        Json::Value fallback_root(Json::objectValue);
        if (root.isObject() && root.isMember("levels_fallback") &&
            root["levels_fallback"].isArray()) {
            fallback_root["levels"] = root["levels_fallback"];
        }
        parse_levels(fallback_root);
    }

    if (favor_text_levels_.empty()) {
        auto push_level = [&](int min, const std::string &hearts,
                              std::initializer_list<std::string> texts) {
            favor_text_level one;
            one.min = min;
            one.hearts = hearts;
            one.texts.assign(texts.begin(), texts.end());
            favor_text_levels_.push_back(std::move(one));
        };

        push_level(0, "♡",
                   {"九二对咪还很陌生，正小心翼翼地保持距离。",
                    "九二会悄悄观察咪，但还不敢主动靠近。",
                    "九二现在更习惯安静地待在角落里。"});
        push_level(40, "♡♡",
                   {"九二开始记住咪的声音了。",
                    "九二虽然紧张，但不再总是躲开咪。",
                    "九二会在咪靠近时轻轻抬头看一眼。"});
        push_level(100, "♡♡♡",
                   {"九二对咪的戒备明显少了很多。",
                    "九二已经愿意在咪身边多停留一会儿。",
                    "九二的眼神里出现了一点点安心。"});
        push_level(200, "♡♡♡♡",
                   {"九二把咪当成可以依赖的人。",
                    "在咪身边时，九二会放松下来。",
                    "九二开始期待和咪一起度过今天。"});
        push_level(350, "♡♡♡♡♡",
                   {"九二会主动来找咪贴贴。", "九二对咪的亲近已经藏不住了。",
                    "九二喜欢跟在咪身边转来转去。"});
        push_level(550, "♡♡♡♡♡♡",
                   {"九二很在意咪的一举一动。", "只要咪在，九二就会觉得安心。",
                    "九二已经把咪当成最特别的人。"});
        push_level(800, "♡♡♡♡♡♡♡",
                   {"九二眼里只剩下咪，连尾巴都在开心摇晃。",
                    "九二会第一时间跑来回应咪的呼唤。",
                    "九二想把每天最好的心情都留给咪。"});
        push_level(1200, "♡♡♡♡♡♡♡♡",
                   {"九二把咪视作生命里独一无二的存在。",
                    "九二只要待在咪身边，就会感到满满的幸福。",
                    "九二与咪之间，已经是深深的双向依恋。"});
    }

    std::sort(favor_text_levels_.begin(), favor_text_levels_.end(),
              [](const favor_text_level &a, const favor_text_level &b) {
                  return a.min < b.min;
              });
}

void rua::recompute_random_mode_unlocked()
{
    random_favor_mode_ = !items_.empty();
    for (const auto &it : items_) {
        if (it.favor != 1) {
            random_favor_mode_ = false;
            break;
        }
    }
}

void rua::load_favor_storage()
{
    favor_by_user_.clear();

    Json::Value root = string_to_json(readfile(rua_state_path(), "{}"));
    Json::Value J;
    if (root.isObject() && root.isMember("favor") && root["favor"].isObject()) {
        J = root["favor"];
    }

    for (const auto &key : J.getMemberNames()) {
        userid_t uid = my_string2uint64(key);
        favor_by_user_[uid] = J[key].asInt64();
    }
}

void rua::save_favor_storage_unlocked() const
{
    Json::Value root = string_to_json(readfile(rua_state_path(), "{}"));
    if (!root.isObject()) {
        root = Json::Value(Json::objectValue);
    }

    Json::Value J(Json::objectValue);
    for (const auto &kv : favor_by_user_) {
        J[std::to_string(kv.first)] = (Json::Int64)kv.second;
    }
    root["favor"] = J;
    writefile(rua_state_path(), root.toStyledString(), false);
}

void rua::load_daily_limit_storage()
{
    daily_rua_count_by_user_.clear();
    daily_rua_date_ = today_date_string();

    Json::Value root = string_to_json(readfile(rua_state_path(), "{}"));
    Json::Value J;
    if (root.isObject() && root.isMember("daily") && root["daily"].isObject()) {
        J = root["daily"];
    }

    if (!J.isObject()) {
        return;
    }

    if (J.isMember("date") && J["date"].isString()) {
        daily_rua_date_ = J["date"].asString();
    }
    if (J.isMember("counts") && J["counts"].isObject()) {
        for (const auto &k : J["counts"].getMemberNames()) {
            daily_rua_count_by_user_[my_string2uint64(k)] =
                J["counts"][k].asInt();
        }
    }

    reset_daily_limit_if_needed_unlocked();
}

void rua::save_daily_limit_storage_unlocked() const
{
    Json::Value root = string_to_json(readfile(rua_state_path(), "{}"));
    if (!root.isObject()) {
        root = Json::Value(Json::objectValue);
    }

    Json::Value daily(Json::objectValue);
    Json::Value counts(Json::objectValue);
    for (const auto &kv : daily_rua_count_by_user_) {
        counts[std::to_string(kv.first)] = kv.second;
    }
    daily["date"] = daily_rua_date_;
    daily["counts"] = counts;
    root["daily"] = daily;
    writefile(rua_state_path(), root.toStyledString(), false);
}

void rua::reset_daily_limit_if_needed_unlocked()
{
    const std::string today = today_date_string();
    if (daily_rua_date_ != today) {
        daily_rua_date_ = today;
        daily_rua_count_by_user_.clear();
        save_daily_limit_storage_unlocked();
    }
}

int rua::pick_favor_delta(const rua_item &item) const
{
    if (random_favor_mode_) {
        // Temporary mode for all-1 datasets: generate a random positive gain.
        return get_random(1, 6);
    }
    return item.favor;
}

const favor_text_level &rua::pick_favor_level(int64_t total) const
{
    if (favor_text_levels_.empty()) {
        static const favor_text_level fallback{0, "♡", {"九二在发呆。"}};
        return fallback;
    }

    const favor_text_level *ret = &favor_text_levels_.front();
    for (const auto &lv : favor_text_levels_) {
        if (total >= lv.min) {
            ret = &lv;
        }
    }
    return *ret;
}

std::string rua::pick_random_alt(const std::vector<std::string> &alts,
                                 const std::string &fallback) const
{
    if (!alts.empty()) {
        return alts[get_random((int)alts.size())];
    }
    return fallback;
}

std::string rua::format_query_favor_text(int64_t total) const
{
    const auto &lv = pick_favor_level(total);
    std::ostringstream oss;
    oss << "当前九二对咪的好感度：" << lv.hearts;
    std::string line = pick_random_alt(lv.texts, "");
    if (!line.empty()) {
        oss << "\n" << line;
    }
    return oss.str();
}

std::string rua::format_draw_favor_text(int64_t total) const
{
    const auto &lv = pick_favor_level(total);
    std::ostringstream oss;
    oss << "当前九二对咪的好感度：" << lv.hearts;
    std::string line = pick_random_alt(lv.texts, "");
    if (!line.empty()) {
        oss << "\n" << line;
    }
    return oss.str();
}

bool rua::is_admin(const msg_meta &conf) const
{
    if (conf.p->is_op(conf.user_id)) {
        return true;
    }
    if (conf.message_type == "group") {
        return is_group_op(conf.p, conf.group_id, conf.user_id);
    }
    return false;
}

int rua::find_item_index_by_name(const std::string &name) const
{
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].name == name) {
            return (int)i;
        }
    }
    return -1;
}

std::string rua::parse_image_url(std::string message,
                                 const msg_meta &conf) const
{
    std::string img;
    if (message.find("[CQ:reply") != std::string::npos) {
        size_t pos = message.find("[CQ:reply,id=");
        size_t end_pos = message.find("]", pos);
        if (pos != std::string::npos && end_pos != std::string::npos) {
            std::string reply_id_str =
                message.substr(pos + 13, end_pos - (pos + 13));
            int64_t reply_id = my_string2int64(reply_id_str);
            Json::Value J;
            J["message_id"] = reply_id;
            std::string res = conf.p->cq_send("get_msg", J);
            Json::Value res_json = string_to_json(res);
            if (res_json.isMember("data") &&
                res_json["data"].isMember("message")) {
                std::string reply_message =
                    messageArr_to_string(res_json["data"]["message"]);
                size_t img_pos = reply_message.find("[CQ:image");
                if (img_pos != std::string::npos) {
                    size_t url_pos = reply_message.find(",url=", img_pos);
                    if (url_pos != std::string::npos) {
                        size_t start = url_pos + 5;
                        size_t comma = reply_message.find(',', start);
                        size_t bracket = reply_message.find(']', start);
                        size_t end = std::min(
                            comma == std::string::npos ? bracket : comma,
                            bracket == std::string::npos ? comma : bracket);
                        if (end != std::string::npos && end > start) {
                            return reply_message.substr(start, end - start);
                        }
                    }
                }
            }
        }
    }

    size_t img_pos = message.find("[CQ:image");
    if (img_pos != std::string::npos) {
        size_t url_pos = message.find(",url=", img_pos);
        if (url_pos != std::string::npos) {
            size_t start = url_pos + 5;
            size_t comma = message.find(',', start);
            size_t bracket = message.find(']', start);
            size_t end =
                std::min(comma == std::string::npos ? bracket : comma,
                         bracket == std::string::npos ? comma : bracket);
            if (end != std::string::npos && end > start) {
                img = message.substr(start, end - start);
            }
        }
    }
    return img;
}

std::string rua::save_image_from_message(const std::string &message,
                                         const msg_meta &conf) const
{
    std::string url = parse_image_url(message, conf);
    if (url.empty()) {
        return "";
    }

    const std::string fallback_file =
        extract_cq_param_from_message(message, "file");
    std::string ext = pick_extension_from_url(url);
    if (ext == ".png" && !fallback_file.empty()) {
        const std::string ext_from_file =
            pick_extension_from_url(fallback_file);
        if (!ext_from_file.empty() && ext_from_file != ".png") {
            ext = ext_from_file;
        }
    }

    const std::string id = generate_uuid();
    const std::string tmp_name = id + ".tmp";
    const fs::path tmp_path = rua_resource_dir() / tmp_name;
    download(cq_decode(url), rua_resource_dir().string() + "/", tmp_name);

    std::string real_ext;
    try {
        Magick::Image img;
        img.read(tmp_path.string());
        real_ext = pick_extension_from_magick_format(img.magick());
    }
    catch (...) {
    }

    if (real_ext.empty()) {
        real_ext = ext.empty() ? ".png" : ext;
    }

    const std::string fname = id + real_ext;
    const fs::path final_path = rua_resource_dir() / fname;
    std::error_code ec;
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        fs::copy_file(tmp_path, final_path,
                      fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_path, ec);
    }
    return fname;
}

std::pair<size_t, size_t> rua::target_image_size_from_kunkun() const
{
    // Keep anti-spam behavior while avoiding heavy blur from tiny downsizing.
    return {520, 520};
}

void rua::cleanup_orphan_images_unlocked()
{
    std::set<std::string> referenced;
    for (const auto &it : items_) {
        const std::string f = trim(it.image);
        if (f.empty()) {
            continue;
        }
        if (is_external_image_ref(f)) {
            continue;
        }
        fs::path p(f);
        if (p.is_absolute()) {
            continue;
        }
        referenced.insert(p.filename().string());
    }

    const fs::path root = rua_resource_dir();
    if (!fs::exists(root)) {
        return;
    }

    const fs::path normalized_dir = root / ".normalized";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!(entry.is_regular_file() || entry.is_symlink())) {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (referenced.find(filename) != referenced.end()) {
            continue;
        }

        fs::remove(entry.path(), ec);
        ec.clear();

        const std::string stem = entry.path().stem().string();
        if (fs::exists(normalized_dir)) {
            for (const auto &ext : {".png", ".gif", ".jpg", ".webp"}) {
                fs::remove(normalized_dir / (stem + "_small" + ext), ec);
                ec.clear();
            }
        }
    }
}

std::string rua::normalized_image_file(const std::string &raw_file,
                                       bool refresh) const
{
    const std::string file = trim(raw_file);
    if (file.empty()) {
        return "";
    }

    fs::path src = rua_resource_dir() / file;
    if (!fs::exists(src) || !fs::is_regular_file(src)) {
        return "";
    }

    fs::path out_dir = rua_resource_dir() / ".normalized";
    fs::create_directories(out_dir);
    std::string ext = src.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool is_gif = (ext == ".gif");
    fs::path out = out_dir / (src.stem().string() +
                              (is_gif ? "_small.gif" : "_small.png"));

    bool need_refresh = refresh || !fs::exists(out);
    if (!need_refresh) {
        std::error_code ec;
        need_refresh =
            fs::last_write_time(out, ec) < fs::last_write_time(src, ec);
    }

    if (need_refresh) {
        try {
            const auto [tw, th] = target_image_size_from_kunkun();

            if (is_gif) {
                std::list<Magick::Image> frames;
                std::list<Magick::Image> coalesced;
                Magick::readImages(&frames, src.string());
                if (frames.empty()) {
                    return "";
                }
                Magick::coalesceImages(&coalesced, frames.begin(),
                                       frames.end());
                if (coalesced.empty()) {
                    return "";
                }

                const size_t w = coalesced.front().columns();
                const size_t h = coalesced.front().rows();
                if (w > 0 && h > 0 && (w > tw || h > th)) {
                    const double ratio = std::min(
                        static_cast<double>(tw) / static_cast<double>(w),
                        static_cast<double>(th) / static_cast<double>(h));
                    const size_t nw = std::max<size_t>(
                        1, static_cast<size_t>(std::floor(w * ratio)));
                    const size_t nh = std::max<size_t>(
                        1, static_cast<size_t>(std::floor(h * ratio)));
                    for (auto &frame : coalesced) {
                        frame.filterType(Magick::LanczosFilter);
                        frame.resize(Magick::Geometry(nw, nh));
                        frame.magick("GIF");
                    }
                }

                std::list<Magick::Image> optimized;
                Magick::optimizeImageLayers(&optimized, coalesced.begin(),
                                            coalesced.end());
                Magick::writeImages(optimized.begin(), optimized.end(),
                                    out.string());
            }
            else {
                Magick::Image img;
                img.read(src.string());
                size_t w = img.columns();
                size_t h = img.rows();
                if (w > 0 && h > 0 && (w > tw || h > th)) {
                    const double ratio = std::min(
                        static_cast<double>(tw) / static_cast<double>(w),
                        static_cast<double>(th) / static_cast<double>(h));
                    const size_t nw = std::max<size_t>(
                        1, static_cast<size_t>(std::floor(w * ratio)));
                    const size_t nh = std::max<size_t>(
                        1, static_cast<size_t>(std::floor(h * ratio)));
                    img.filterType(Magick::LanczosFilter);
                    img.resize(Magick::Geometry(nw, nh));
                }
                img.write(out.string());
            }
        }
        catch (...) {
            return "";
        }
    }

    return "file://" + fs::absolute(out).string();
}

std::string rua::resolve_image_file(const std::string &raw_file,
                                    bool refresh_normalized) const
{
    std::string file = trim(raw_file);
    if (file.empty()) {
        return "";
    }

    if (is_external_image_ref(file)) {
        return file;
    }

    fs::path raw_path(file);
    if (raw_path.is_absolute()) {
        return "file://" + raw_path.string();
    }

    fs::path under_rua = rua_resource_dir() / raw_path;
    if (fs::exists(under_rua)) {
        std::string normalized =
            normalized_image_file(raw_path.string(), refresh_normalized);
        if (!normalized.empty()) {
            return normalized;
        }
        return "file://" + fs::absolute(under_rua).string();
    }

    fs::path rel = fs::path("./") / raw_path;
    if (fs::exists(rel)) {
        return "file://" + fs::absolute(rel).string();
    }

    return "";
}

void rua::process(std::string message, const msg_meta &conf)
{
    std::string m = trim(message);
    bool admin = is_admin(conf);

    std::lock_guard<std::mutex> guard(mutex_);
    sync_dirs_from_bot(conf.p);

    const cmd_middleware_t admin_only = [admin]() { return admin; };

    auto handle_get = [&](const std::string &name) {
        int idx = find_item_index_by_name(name);
        if (idx < 0) {
            conf.p->cq_send("未找到该 rua 名称", conf);
            return true;
        }

        const rua_item &it = items_[idx];
        std::ostringstream oss;
        oss << "name: " << it.name << "\n"
            << "description: " << it.description << "\n"
            << "image: " << it.image << "\n"
            << "favor: " << it.favor;
        const std::string image_ref = resolve_image_file(it.image, true);
        if (!image_ref.empty()) {
            oss << "\n[CQ:image,file=" << image_ref << ",id=40000]";
        }
        conf.p->cq_send(oss.str(), conf);
        return true;
    };

    auto handle_add = [&](const std::string &payload) {
        std::vector<std::string> parts = split_by_char(payload, '|');
        if (parts.size() < 2) {
            conf.p->cq_send(
                "格式: *rua.add 名称|描述|favor(可选)，然后发送图片", conf);
            return true;
        }

        rua_item one;
        one.name = trim(parts[0]);
        one.description = trim(parts[1]);
        one.favor = parts.size() >= 3 ? (int)my_string2int64(parts[2]) : 1;
        if (one.name.empty()) {
            conf.p->cq_send("名称不能为空", conf);
            return true;
        }

        std::string image_name;
        try {
            image_name = save_image_from_message(m, conf);
        }
        catch (...) {
            conf.p->cq_send("图片下载失败，请重试", conf);
            return true;
        }

        if (image_name.empty()) {
            pending_add_by_user_[conf.user_id] = one;
            conf.p->cq_send("已记录条目，请直接发送图片或回复一条图片消息",
                            conf);
            return true;
        }

        one.image = image_name;
        int idx = find_item_index_by_name(one.name);
        if (idx >= 0) {
            items_[idx] = one;
        }
        else {
            items_.push_back(one);
        }

        recompute_random_mode_unlocked();
        save_config_unlocked();
        cleanup_orphan_images_unlocked();
        pending_add_by_user_.erase(conf.user_id);
        conf.p->cq_send(fmt::format("rua 已保存: {}", one.name), conf);
        return true;
    };

    auto handle_del = [&](const std::string &name) {
        int idx = find_item_index_by_name(name);
        if (idx < 0) {
            conf.p->cq_send("未找到该 rua 名称", conf);
            return true;
        }
        items_.erase(items_.begin() + idx);

        recompute_random_mode_unlocked();
        save_config_unlocked();
        cleanup_orphan_images_unlocked();
        conf.p->cq_send(fmt::format("rua 已删除: {}", name), conf);
        return true;
    };

    auto handle_draw = [&]() {
        if (items_.empty()) {
            conf.p->cq_send("rua 列表为空，请先编辑 " + rua_config_path(),
                            conf);
            return true;
        }

        const rua_item &pick = items_[get_random((int)items_.size())];
        const userid_t target = conf.user_id;
        const bool op_debug_no_limit = conf.p->is_op(conf.user_id);

        reset_daily_limit_if_needed_unlocked();
        int &daily_count = daily_rua_count_by_user_[target];
        if (!op_debug_no_limit && daily_count >= 1) {
            conf.p->cq_send("今天已经rua过1次啦，明天再来贴贴~", conf);
            return true;
        }

        const int favor_delta = pick_favor_delta(pick);
        const int64_t favor_total = (favor_by_user_[target] += favor_delta);
        if (!op_debug_no_limit) {
            ++daily_count;
        }
        save_favor_storage_unlocked();
        if (!op_debug_no_limit) {
            save_daily_limit_storage_unlocked();
        }

        std::ostringstream oss;
        oss << "恭喜rua到了一只" << pick.name << "！\n";

        const std::string image_ref = resolve_image_file(pick.image, true);
        if (!image_ref.empty()) {
            oss << "[CQ:image,file=" << image_ref << ",id=40000]";
        }

        oss << pick.name;
        if (!pick.description.empty()) {
            oss << "\n" << pick.description;
        }
        oss << "\n\n" << format_draw_favor_text(favor_total);

        conf.p->cq_send(oss.str(), conf);
        return true;
    };

    const std::vector<cmd_exact_rule> exact_rules = {
        {CMD_HELP,
         [&]() {
             help_level_t lv = help_level_t::public_only;
             if (admin) {
                 lv = conf.p->is_op(conf.user_id) ? help_level_t::bot_admin
                                                  : help_level_t::group_admin;
             }
             conf.p->cq_send(rua_detail_help(lv), conf);
             return true;
         }},
        {CMD_RELOAD,
         [&]() {
             load_config();
             load_favor_text_config();
             load_favor_storage();
             load_daily_limit_storage();
             conf.p->cq_send("rua 配置已重载", conf);
             return true;
         },
         {admin_only}},
        {CMD_FAVOR_EN,
         [&]() {
             int64_t total = 0;
             auto it = favor_by_user_.find(conf.user_id);
             if (it != favor_by_user_.end()) {
                 total = it->second;
             }
             conf.p->cq_send(format_query_favor_text(total), conf);
             return true;
         }},
        {CMD_FAVOR_ZH,
         [&]() {
             int64_t total = 0;
             auto it = favor_by_user_.find(conf.user_id);
             if (it != favor_by_user_.end()) {
                 total = it->second;
             }
             conf.p->cq_send(format_query_favor_text(total), conf);
             return true;
         }},
        {CMD_LIST,
         [&]() {
             if (items_.empty()) {
                 conf.p->cq_send("rua 列表为空", conf);
                 return true;
             }

             std::ostringstream oss;
             oss << "rua 名称列表(" << items_.size() << "):\n";
             for (size_t i = 0; i < items_.size(); ++i) {
                 oss << i + 1 << ". " << items_[i].name << "\n";
             }
             conf.p->cq_send(trim(oss.str()), conf);
             return true;
         },
         {admin_only}},
        {CMD_DRAW, handle_draw},
    };

    const std::vector<cmd_prefix_rule> prefix_rules = {
        {CMD_GET,
         [&]() {
             std::string name;
             if (!cmd_strip_prefix(m, CMD_GET, name)) {
                 return false;
             }
             return handle_get(name);
         },
         {admin_only}},
        {CMD_ADD,
         [&]() {
             std::string payload;
             if (!cmd_strip_prefix(m, CMD_ADD, payload)) {
                 return false;
             }
             return handle_add(payload);
         },
         {admin_only}},
        {CMD_DEL,
         [&]() {
             std::string name;
             if (!cmd_strip_prefix(m, CMD_DEL, name)) {
                 return false;
             }
             return handle_del(name);
         },
         {admin_only}},
    };

    bool handled = false;
    (void)cmd_try_dispatch(m, exact_rules, prefix_rules, handled);
    if (handled) {
        return;
    }

    if (admin) {
        auto pit = pending_add_by_user_.find(conf.user_id);
        if (pit != pending_add_by_user_.end()) {
            std::string image_name;
            try {
                image_name = save_image_from_message(m, conf);
            }
            catch (...) {
                conf.p->cq_send("图片下载失败，请重试", conf);
                return;
            }

            if (image_name.empty()) {
                conf.p->cq_send("未检测到图片，请发送图片或回复一条图片消息",
                                conf);
                return;
            }

            rua_item one = pit->second;
            one.image = image_name;
            int idx = find_item_index_by_name(one.name);
            if (idx >= 0) {
                items_[idx] = one;
            }
            else {
                items_.push_back(one);
            }
            recompute_random_mode_unlocked();
            save_config_unlocked();
            cleanup_orphan_images_unlocked();
            pending_add_by_user_.erase(pit);
            conf.p->cq_send(fmt::format("rua 已保存: {}", one.name), conf);
            return;
        }
    }

    if (items_.empty()) {
        conf.p->cq_send("rua 列表为空，请先编辑 " + rua_config_path(), conf);
    }
}

bool rua::check(std::string message, const msg_meta &conf)
{
    std::string m = trim(message);
    if (cmd_match_exact(m, {CMD_HELP, CMD_RELOAD, CMD_FAVOR_EN, CMD_FAVOR_ZH,
                            CMD_LIST, CMD_DRAW}) ||
        cmd_match_prefix(
            m, {std::string(CMD_DRAW) + " ", CMD_GET, CMD_ADD, CMD_DEL})) {
        return true;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    return pending_add_by_user_.find(conf.user_id) !=
           pending_add_by_user_.end();
}

std::string rua::help()
{
    return "九二rua：抽取互动并累计好感。详细帮助：*rua.help";
}

std::string rua::help(const msg_meta &conf, help_level_t level)
{
    (void)conf;
    if (level == help_level_t::group_admin ||
        level == help_level_t::bot_admin) {
        return "九二rua：抽取互动并累计好感。帮助：*rua.help（含管理员命令）";
    }
    return help();
}

DECLARE_FACTORY_FUNCTIONS(rua)
