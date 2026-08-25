// dateimg_util.h — small inline helpers shared across the dateimg .cpp files.
#pragma once

#include "utils.h" // trim, cq_decode

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace dutil {

inline constexpr char CMD_PREFIX[] = "dateimg.";

// accepted background image extensions
inline bool has_image_ext(const std::string &s) {
    auto ends = [&](const char *ext) {
        const size_t n = std::string(ext).size();
        if (s.size() < n) {
            return false;
        }
        std::string e = s.substr(s.size() - n);
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return e == ext;
    };
    return ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".webp");
}

// keep only filesystem/CQ-safe chars for output/upload filenames
inline std::string safe_stem(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
            c == '-') {
            o.push_back(c);
        } else {
            o.push_back('_');
        }
    }
    if (o.empty()) {
        o = "bg";
    }
    return o;
}

// admin-supplied upload name -> safe "<name>.png" key (uploads are transcoded)
inline std::string bg_key_from_name(const std::string &raw) {
    std::string n = trim(raw);
    for (const char *ext : {".png", ".jpg", ".jpeg", ".webp"}) {
        const size_t k = std::string(ext).size();
        if (n.size() >= k) {
            std::string e = n.substr(n.size() - k);
            std::transform(e.begin(), e.end(), e.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (e == ext) {
                n.resize(n.size() - k); // strip the extension
                break;
            }
        }
    }
    return safe_stem(n) + ".png";
}

inline bool has_image(const std::string &message) {
    return message.find("[CQ:image,") != std::string::npos;
}

// escape a plain-text run so it can be concatenated with raw CQ codes without a
// stray & / [ / ] being parsed as a CQ segment. (& must be replaced first.)
inline std::string cq_escape_text(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '[':
            o += "&#91;";
            break;
        case ']':
            o += "&#93;";
            break;
        default:
            o.push_back(c);
        }
    }
    return o;
}

// pull the (cq-decoded) url out of the first [CQ:image,...] segment
inline bool parse_image_url(const std::string &message, std::string &url_out) {
    size_t idx = message.find("[CQ:image,");
    if (idx == std::string::npos) {
        return false;
    }
    size_t u = message.find(",url=", idx);
    if (u == std::string::npos) {
        return false;
    }
    u += 5; // strlen(",url=")
    size_t e = message.find_first_of(",]", u);
    if (e == std::string::npos) {
        return false;
    }
    url_out = cq_decode(message.substr(u, e - u));
    return !url_out.empty();
}

// parse a month/day date: "8.10" / "8月10日" / "0810" -> mm,dd. false on
// failure.
inline bool parse_md(const std::string &raw, int &mm, int &dd) {
    std::string t = trim(raw);
    size_t dot = t.find_first_of(".．·-/");
    if (dot != std::string::npos) {
        try {
            mm = std::stoi(t.substr(0, dot));
            dd = std::stoi(t.substr(dot + 1));
            return true;
        } catch (...) {
            return false;
        }
    }
    size_t yp = t.find("月");
    if (yp != std::string::npos) {
        std::string ms = t.substr(0, yp);
        std::string ds = t.substr(yp + std::string("月").size());
        size_t rp = ds.find("日");
        if (rp != std::string::npos) {
            ds.resize(rp); // drop the trailing 日 and anything after
        }
        try {
            mm = std::stoi(ms);
            dd = std::stoi(ds);
            return true;
        } catch (...) {
            return false;
        }
    }
    if (t.size() == 4 &&
        t.find_first_not_of("0123456789") == std::string::npos) {
        try {
            mm = std::stoi(t.substr(0, 2));
            dd = std::stoi(t.substr(2, 2));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

// split "8月22日" -> {"8月", "22日"} for the two-line (square/circle) layout
inline std::pair<std::string, std::string>
split_month_day(const std::string &date) {
    const std::string sep = "月";
    size_t p = date.find(sep);
    if (p == std::string::npos) {
        return {date, ""};
    }
    return {date.substr(0, p + sep.size()), date.substr(p + sep.size())};
}

} // namespace dutil
