#pragma once

#include "bili_card.h"

#include <string>

namespace bili_decode {

// Parse message text and fetch bilibili video metadata by BV/av token.
// Returns empty string when no valid token is found or API returns no data.
std::string decode_video_text(const std::string &input_text);

// Same lookup, filling the video-info picture's fields (2026-11-16: BV codes are answered with a picture).
// Returns false when no valid token is found or the API returns no data.
bool fetch_video_info(const std::string &input_text, bili_card::info_t &info);

} // namespace bili_decode
