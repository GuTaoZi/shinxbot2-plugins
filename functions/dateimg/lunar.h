// lunar.h — Gregorian -> Chinese lunar (农历) day-of-month, self-contained
// C++17.
//
// Algorithm: canonical 1900-2100 lunar-info-table method (jjonline calendar.js;
// same data as Python lunardate / zhdate). Base epoch 1900-01-31 = 农历1900
// 正月初一. We only need the lunar DAY-OF-MONTH (1..30). Header-only, no
// dependencies.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace lunar {

// Per-year packed data, index = year - 1900. Covers 1900..2049.
inline constexpr std::array<uint32_t, 150> kLunarInfo = {{
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260,
    0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, // 1900-1909
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255,
    0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, // 1910-1919
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40,
    0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, // 1920-1929
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0,
    0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, // 1930-1939
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4,
    0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, // 1940-1949
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0,
    0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, // 1950-1959
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570,
    0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, // 1960-1969
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4,
    0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, // 1970-1979
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a,
    0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, // 1980-1989
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50,
    0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0, // 1990-1999
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552,
    0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, // 2000-2009
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9,
    0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, // 2010-2019
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60,
    0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, // 2020-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0,
    0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, // 2030-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577,
    0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, // 2040-2049
}};

constexpr int kBaseYear = 1900;

inline int leapMonth(int y) { return kLunarInfo[y - kBaseYear] & 0xf; }

inline int leapDays(int y) {
    if (leapMonth(y)) {
        return (kLunarInfo[y - kBaseYear] & 0x10000) ? 30 : 29;
    }
    return 0;
}

inline int monthDays(int y, int m) {
    return (kLunarInfo[y - kBaseYear] & (0x10000 >> m)) ? 30 : 29;
}

inline int yearDays(int y) {
    int sum = 348; // 12 * 29
    for (int i = 0x8000; i > 0x8; i >>= 1) {
        sum += (kLunarInfo[y - kBaseYear] & i) ? 1 : 0;
    }
    return sum + leapDays(y);
}

// Days since 1970-01-01 (proleptic Gregorian). Howard Hinnant's algorithm.
inline long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long>(era) * 146097 + static_cast<int>(doe) - 719468;
}

struct LunarDate {
    int year;
    int month; // 1..12
    int day;   // 1..30
    bool isLeap;
    bool valid;
};

// Convert Gregorian y/m/d -> lunar. valid=false if outside the table range.
inline LunarDate solar2lunar(int y, int m, int d) {
    LunarDate r{0, 0, 0, false, false};
    long offset = daysFromCivil(y, m, d) - daysFromCivil(1900, 1, 31);
    if (offset < 0) {
        return r; // before the epoch
    }

    int i, temp = 0;
    for (i = kBaseYear;
         i < kBaseYear + static_cast<int>(kLunarInfo.size()) && offset > 0;
         ++i) {
        temp = yearDays(i);
        offset -= temp;
    }
    if (offset < 0) {
        offset += temp;
        --i;
    }
    const int lunarYear = i;
    if (lunarYear >= kBaseYear + static_cast<int>(kLunarInfo.size())) {
        return r; // past the table
    }

    const int leap = leapMonth(lunarYear);
    bool isLeap = false;

    for (i = 1; i < 13 && offset > 0; ++i) {
        if (leap > 0 && i == (leap + 1) && !isLeap) {
            --i;
            isLeap = true;
            temp = leapDays(lunarYear);
        } else {
            temp = monthDays(lunarYear, i);
        }
        if (isLeap && i == (leap + 1)) {
            isLeap = false;
        }
        offset -= temp;
    }
    if (offset == 0 && leap > 0 && i == leap + 1) {
        if (isLeap) {
            isLeap = false;
        } else {
            isLeap = true;
            --i;
        }
    }
    if (offset < 0) {
        offset += temp;
        --i;
    }

    r.year = lunarYear;
    r.month = i;
    r.day = static_cast<int>(offset) + 1;
    r.isLeap = isLeap;
    r.valid = true;
    return r;
}

// Lunar day-of-month (1..30) as traditional 大写 numerals, no 初 prefix:
//   1->壹 8->捌 10->拾 18->拾捌 20->廿 28->廿捌 30->卅  (matches user's
//   examples)
inline std::string day_numeral(int day) {
    static const char *dig[] = {"",   "壹", "貳", "參", "肆",
                                "伍", "陸", "柒", "捌", "玖"};
    if (day < 1 || day > 30) {
        return "";
    }
    if (day < 10) {
        return dig[day];
    }
    if (day == 10) {
        return "拾";
    }
    if (day < 20) {
        return std::string("拾") + dig[day - 10];
    }
    if (day == 20) {
        return "廿";
    }
    if (day < 30) {
        return std::string("廿") + dig[day - 20];
    }
    return "卅"; // 30
}

// Convenience: traditional-numeral lunar day for a Gregorian date ("" if out of
// range).
inline std::string day_numeral_of(int y, int m, int d) {
    LunarDate ld = solar2lunar(y, m, d);
    return ld.valid ? day_numeral(ld.day) : std::string();
}

// Conventional lunar month name: 正月 二月 … 十月 十一月 十二月 (闰 prefix for
// a leap month).
inline std::string month_name(int m, bool leap) {
    static const char *mo[] = {"",     "正月",   "二月",  "三月", "四月",
                               "五月", "六月",   "七月",  "八月", "九月",
                               "十月", "十一月", "十二月"};
    if (m < 1 || m > 12) {
        return "";
    }
    return std::string(leap ? "闰" : "") + mo[m];
}

// Conventional lunar day name: 初一…初十 十一…十九 二十 廿一…廿九 三十
inline std::string day_name(int d) {
    static const char *n[] = {"",   "一", "二", "三", "四", "五",
                              "六", "七", "八", "九", "十"};
    if (d < 1 || d > 30) {
        return "";
    }
    if (d <= 10) {
        return std::string("初") + (d == 10 ? "十" : n[d]);
    }
    if (d < 20) {
        return std::string("十") + n[d - 10];
    }
    if (d == 20) {
        return "二十";
    }
    if (d < 30) {
        return std::string("廿") + n[d - 20];
    }
    return "三十";
}

// Full conventional lunar date for a Gregorian date, e.g. "八月初十" /
// "闰四月廿八"
// ("" if out of table range).
inline std::string date_cn(int y, int m, int d) {
    LunarDate ld = solar2lunar(y, m, d);
    if (!ld.valid) {
        return "";
    }
    return month_name(ld.month, ld.isLeap) + day_name(ld.day);
}

} // namespace lunar
