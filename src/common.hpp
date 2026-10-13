#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <thread>
#include <cctype>

namespace chokidar {

// Only sessions located in India are ever candidates for termination; anything
// outside India is always kept. Inside India, these cities are ALWAYS protected
// and can never be removed by --allow (--allow only adds more).
inline const std::vector<std::string> PROTECTED_LOCATIONS = {
    "mumbai", "navi mumbai", "panvel", "thane"
};

// Country suffixes Claude may append for India ("Pune, Maharashtra, IN").
inline const std::vector<std::string> INDIA_COUNTRY_NAMES = {
    "in", "india"
};

inline const std::string DEFAULT_CHROME_PROFILE = "shreyans.tatiya@gmail.com";
inline const std::string SETTINGS_URL = "https://claude.ai/settings/account";
inline const int DEFAULT_INTERVAL_SECONDS = 60;
inline const int REAL_PROFILE_DEBUG_PORT = 9223;
inline const int BOT_PROFILE_DEBUG_PORT = 9222;

inline std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

inline std::string to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

inline std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline std::string replace_all(std::string str, const std::string& from, const std::string& to) {
    if (from.empty()) return str;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

inline std::string collapse_whitespace(const std::string& str) {
    std::string res;
    bool in_space = false;
    for (char c : str) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_space) {
                res += ' ';
                in_space = true;
            }
        } else {
            res += c;
            in_space = false;
        }
    }
    return trim(res);
}

inline std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now_c);
#else
    localtime_r(&now_c, &tm_buf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return std::string(buf);
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

inline void sleep_ms(int ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

} // namespace chokidar
