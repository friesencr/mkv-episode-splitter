#pragma once
#include <string>
#include <cstdint>

namespace mkvsplit {

// Convert timestamp in seconds (e.g. 125.45) to HH:MM:SS.mmm formatted string
std::string SecondsToTimeString(double seconds);

// Parse timestamp string ("HH:MM:SS.mmm", "MM:SS.mmm", or "SS.mmm") to seconds
double TimeStringToSeconds(const std::string& time_str);

// Format file size in human-readable string (e.g. "450.5 MB")
std::string FormatBytes(uint64_t bytes);

// Format duration in human readable format (e.g. "01h 15m 30s")
std::string FormatDuration(double seconds);

} // namespace mkvsplit
