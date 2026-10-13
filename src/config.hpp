#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <map>

namespace chokidar {

struct SessionCookie {
    std::string name;
    std::string value;
    std::string domain = ".claude.ai";
    std::string path = "/";
    bool http_only = true;
    bool secure = true;
};

struct Options {
    bool dry_run = false;
    std::vector<std::string> allowed_locations = PROTECTED_LOCATIONS;
    int cdp_port = 0;
    std::string user_data_dir = "./chrome_session";
    bool clean_session = false;
    bool headless = false;
    int interval = DEFAULT_INTERVAL_SECONDS;
    bool once = false;
    bool help = false;
    std::string chrome_path;
    std::string chrome_profile = DEFAULT_CHROME_PROFILE;
    bool use_real_profile = true;
    std::string session_key;
    bool list_profiles = false;
    int64_t bot_started_at = 0;
    std::string save_session_key_arg;
    bool launch_chrome_debug = false;
};

// Parse an interval string like "300", "90s", "7m" or "2h" into seconds. Returns -1 if invalid.
int parse_interval(const std::string& value);

// Parse cookies from bare string, header string, or multi-line string.
std::vector<SessionCookie> parse_session_cookies(const std::string& raw);

// Minimal .env reader: KEY=value lines, # comments, optional quotes, BOM strip.
std::map<std::string, std::string> load_dot_env(const std::string& file_path);

// Read cookies considering order of precedence.
std::vector<SessionCookie> read_session_cookies(const std::string& explicit_key, const std::string& base_dir = ".");

// Helper to get first cookie value.
std::string read_session_key(const std::string& explicit_key, const std::string& base_dir = ".");

// Save cookies to session-key.txt.
bool save_session_key_file(const std::string& raw, const std::string& file_path = "session-key.txt");

// Parse command line arguments.
Options parse_args(int argc, char* argv[]);

// Print usage help.
void show_help(const char* prog = "claude-chokidar");

} // namespace chokidar
