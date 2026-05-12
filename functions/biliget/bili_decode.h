#pragma once

#include <string>

namespace biliget_decode {

// Parse message text and fetch bilibili video metadata by BV/av token.
// Returns empty string when no valid token is found or API returns no data.
std::string decode_video_text(const std::string &input_text);

} // namespace biliget_decode
