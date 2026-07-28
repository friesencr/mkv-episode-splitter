#include "mkvsplit/utils.hpp"
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <vector>

namespace mkvsplit {

std::string SecondsToTimeString(double total_seconds) {
    if (total_seconds < 0) total_seconds = 0;
    int hours = static_cast<int>(total_seconds) / 3600;
    int minutes = (static_cast<int>(total_seconds) % 3600) / 60;
    int seconds = static_cast<int>(total_seconds) % 60;
    int millis = static_cast<int>(std::round((total_seconds - std::floor(total_seconds)) * 1000.0));

    if (millis >= 1000) {
        millis = 0;
        seconds += 1;
        if (seconds >= 60) {
            seconds = 0;
            minutes += 1;
            if (minutes >= 60) {
                minutes = 0;
                hours += 1;
            }
        }
    }

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds << "."
        << std::setfill('0') << std::setw(3) << millis;
    return oss.str();
}

double TimeStringToSeconds(const std::string& time_str) {
    if (time_str.empty()) return 0.0;

    // Check if it's purely a numeric string in seconds
    try {
        size_t parsed_len = 0;
        double val = std::stod(time_str, &parsed_len);
        if (parsed_len == time_str.length() && time_str.find(':') == std::string::npos) {
            return val;
        }
    } catch (...) {}

    // Parse HH:MM:SS.mmm or MM:SS.mmm
    std::vector<std::string> parts;
    std::stringstream ss(time_str);
    std::string token;
    while (std::getline(ss, token, ':')) {
        parts.push_back(token);
    }

    double total_sec = 0.0;
    if (parts.size() == 3) {
        total_sec += std::stod(parts[0]) * 3600.0; // HH
        total_sec += std::stod(parts[1]) * 60.0;   // MM
        total_sec += std::stod(parts[2]);          // SS.mmm
    } else if (parts.size() == 2) {
        total_sec += std::stod(parts[0]) * 60.0;   // MM
        total_sec += std::stod(parts[1]);          // SS.mmm
    } else if (parts.size() == 1) {
        total_sec = std::stod(parts[0]);
    }
    return total_sec;
}

std::string FormatBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double count = static_cast<double>(bytes);

    while (count >= 1024.0 && unit_idx < 4) {
        count /= 1024.0;
        unit_idx++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << count << " " << units[unit_idx];
    return oss.str();
}

std::string FormatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    int hours = static_cast<int>(seconds) / 3600;
    int minutes = (static_cast<int>(seconds) % 3600) / 60;
    int secs = static_cast<int>(seconds) % 60;

    std::ostringstream oss;
    if (hours > 0) {
        oss << hours << "h " << std::setfill('0') << std::setw(2) << minutes << "m " << std::setfill('0') << std::setw(2) << secs << "s";
    } else if (minutes > 0) {
        oss << minutes << "m " << std::setfill('0') << std::setw(2) << secs << "s";
    } else {
        oss << secs << "s";
    }
    return oss.str();
}

std::string TruncateString(const std::string& str, size_t max_len) {
    if (str.length() <= max_len) return str;
    if (max_len <= 3) return str.substr(0, max_len);
    size_t half = (max_len - 3) / 2;
    return str.substr(0, half) + "..." + str.substr(str.length() - (max_len - 3 - half));
}

} // namespace mkvsplit
