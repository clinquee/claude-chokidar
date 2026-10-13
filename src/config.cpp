#include "config.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <sys/stat.h>

namespace chokidar {

int parse_interval(const std::string& value) {
    std::string s = trim(value);
    if (s.empty()) return -1;
    std::regex re(R"(^(\d+)\s*([smh]?)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(s, match, re)) {
        return -1;
    }
    int64_t num = 0;
    try {
        num = std::stoll(match[1].str());
    } catch (...) {
        return -1;
    }
    std::string unit = to_lower(match[2].str());
    int64_t multiplier = 1;
    if (unit == "s" || unit.empty()) multiplier = 1;
    else if (unit == "m") multiplier = 60;
    else if (unit == "h") multiplier = 3600;

    int64_t total = num * multiplier;
    if (total <= 0 || total > 86400 * 365) return -1;
    return static_cast<int>(total);
}

std::vector<SessionCookie> parse_session_cookies(const std::string& raw) {
    if (raw.empty()) return {};

    std::string text = raw;
    // Strip UTF-8 or UTF-16 BOM if present
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text = text.substr(3);
    } else if (text.size() >= 2 && ((unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE)) {
        text = text.substr(2);
    } else if (text.size() >= 2 && ((unsigned char)text[0] == 0xFE && (unsigned char)text[1] == 0xFF)) {
        text = text.substr(2);
    }
    // Strip null bytes
    std::string sanitized;
    sanitized.reserve(text.size());
    for (char c : text) {
        if (c != '\0') sanitized.push_back(c);
    }

    std::vector<std::string> chunks;
    std::string cur;
    for (char c : sanitized) {
        if (c == '\r' || c == '\n' || c == ';') {
            std::string t = trim(cur);
            if (!t.empty()) chunks.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        std::string t = trim(cur);
        if (!t.empty()) chunks.push_back(t);
    }

    std::vector<std::pair<std::string, std::string>> raw_cookies;
    std::regex kv_re(R"(^["']?([A-Za-z0-9_.\-]+)["']?\s*[=\t:]\s*["']?(.+?)["']?$)");

    for (const auto& chunk : chunks) {
        if (chunk.empty() || chunk[0] == '#') continue;
        std::smatch m;
        if (std::regex_match(chunk, m, kv_re)) {
            raw_cookies.emplace_back(m[1].str(), trim(m[2].str()));
        } else if (chunk.find(' ') == std::string::npos && chunk.find('\t') == std::string::npos) {
            std::string val = chunk;
            if (val.front() == '"' || val.front() == '\'') val = val.substr(1);
            if (!val.empty() && (val.back() == '"' || val.back() == '\'')) val.pop_back();
            raw_cookies.emplace_back("sessionKey", val);
        }
    }

    // Keep only sessionKey cookies, last occurrence of each name wins
    std::map<std::string, std::string> by_name;
    for (const auto& kv : raw_cookies) {
        std::string name_lower = to_lower(kv.first);
        if (name_lower.rfind("sessionkey", 0) == 0 && !kv.second.empty()) {
            by_name[kv.first] = kv.second;
        }
    }

    std::vector<SessionCookie> out;
    for (const auto& pair : by_name) {
        out.push_back(SessionCookie{pair.first, pair.second, ".claude.ai", "/", true, true});
    }
    return out;
}

std::map<std::string, std::string> load_dot_env(const std::string& file_path) {
    std::map<std::string, std::string> env;
    std::ifstream file(file_path);
    if (!file.is_open()) return env;

    std::string line;
    bool first_line = true;
    while (std::getline(file, line)) {
        if (first_line) {
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                line = line.substr(3);
            }
            first_line = false;
        }
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos || eq == 0) continue;

        std::string key = trim(trimmed.substr(0, eq));
        if (key.rfind("export ", 0) == 0) {
            key = trim(key.substr(7));
        }
        std::string val = trim(trimmed.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (!key.empty()) {
            env[key] = val;
        }
    }
    return env;
}

static std::string cookie_name_from_env_key(const std::string& key) {
    std::regex re(R"(^SESSION_KEY([A-Za-z0-9]*?)(?:_v?\d+)?$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(key, m, re)) {
        return "";
    }
    std::string suffix = m[1].str();
    // uppercase suffix e.g. V3, LC, V3LC
    for (auto& c : suffix) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return "sessionKey" + suffix;
}

static std::vector<SessionCookie> cookies_from_env_map(const std::map<std::string, std::string>& env) {
    std::vector<SessionCookie> cookies;
    for (const auto& pair : env) {
        std::string name = cookie_name_from_env_key(pair.first);
        if (!name.empty() && !pair.second.empty()) {
            cookies.push_back(SessionCookie{name, pair.second, ".claude.ai", "/", true, true});
        }
    }
    return cookies;
}

std::vector<SessionCookie> read_session_cookies(const std::string& explicit_key, const std::string& base_dir) {
    if (!explicit_key.empty()) {
        auto c = parse_session_cookies(explicit_key);
        if (!c.empty()) return c;
    }

    const char* env_var = std::getenv("CLAUDE_SESSION_KEY");
    if (env_var && *env_var) {
        auto c = parse_session_cookies(env_var);
        if (!c.empty()) return c;
    }

    std::string key_file = base_dir + "/session-key.txt";
    std::ifstream kf(key_file);
    if (kf.is_open()) {
        std::stringstream ss;
        ss << kf.rdbuf();
        auto c = parse_session_cookies(ss.str());
        if (!c.empty()) return c;
    }

    std::string env_file = base_dir + "/.env";
    auto dot_env = load_dot_env(env_file);
    auto c = cookies_from_env_map(dot_env);
    if (!c.empty()) return c;

    return {};
}

std::string read_session_key(const std::string& explicit_key, const std::string& base_dir) {
    auto cookies = read_session_cookies(explicit_key, base_dir);
    if (!cookies.empty()) {
        return cookies[0].value;
    }
    return "";
}

bool save_session_key_file(const std::string& raw, const std::string& file_path) {
    auto cookies = parse_session_cookies(raw);
    if (cookies.empty()) {
        std::cerr << "No claude.ai session cookie found in what was provided. Nothing written.\n";
        std::cerr << "Expected a value like sk-ant-sid01-..., or \"sessionKey=sk-ant-sid01-...\".\n";
        return false;
    }

    std::ofstream out(file_path);
    if (!out.is_open()) {
        std::cerr << "Failed to open " << file_path << " for writing.\n";
        return false;
    }

    out << "# claude.ai session cookies - treat this file like a password.\n";
    out << "# Generated by claude-chokidar\n";
    for (const auto& c : cookies) {
        out << c.name << "=" << c.value << "\n";
    }
    out.close();

#if !defined(_WIN32)
    chmod(file_path.c_str(), 0600);
#endif

    std::cout << "Saved " << cookies.size() << " cookie(s) to " << file_path << ":\n";
    bool starts_with_ant = false;
    for (const auto& c : cookies) {
        std::cout << "  " << c.name << " (" << c.value.size() << " chars)\n";
        if (c.value.rfind("sk-ant-sid", 0) == 0) starts_with_ant = true;
    }
    if (!starts_with_ant) {
        std::cout << "\nNote: claude.ai session values normally start with \"sk-ant-sid\".\n";
        std::cout << "If sign-in fails, re-copy the cookie value.\n";
    }
    std::cout << "\nThis file is gitignored.\n";
    return true;
}

Options parse_args(int argc, char* argv[]) {
    Options opts;
    opts.bot_started_at = now_ms();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "--once") {
            opts.once = true;
        } else if (arg == "--interval" || arg == "-i") {
            if (i + 1 < argc) {
                int val = parse_interval(argv[++i]);
                if (val > 0) opts.interval = val;
            }
        } else if (arg.rfind("--interval=", 0) == 0) {
            int val = parse_interval(arg.substr(11));
            if (val > 0) opts.interval = val;
        } else if (arg == "--allow" || arg == "-a") {
            if (i + 1 < argc) {
                std::string val = argv[++i];
                auto parts = split(val, ',');
                for (const auto& p : parts) {
                    std::string s = to_lower(trim(p));
                    if (!s.empty()) opts.allowed_locations.push_back(s);
                }
            }
        } else if (arg.rfind("--allow=", 0) == 0) {
            std::string val = arg.substr(8);
            auto parts = split(val, ',');
            for (const auto& p : parts) {
                std::string s = to_lower(trim(p));
                if (!s.empty()) opts.allowed_locations.push_back(s);
            }
        } else if (arg == "--chrome-path") {
            if (i + 1 < argc) opts.chrome_path = argv[++i];
        } else if (arg.rfind("--chrome-path=", 0) == 0) {
            opts.chrome_path = arg.substr(14);
        } else if (arg == "--chrome-profile") {
            if (i + 1 < argc) opts.chrome_profile = argv[++i];
        } else if (arg.rfind("--chrome-profile=", 0) == 0) {
            opts.chrome_profile = arg.substr(17);
        } else if (arg == "--session-key") {
            if (i + 1 < argc) opts.session_key = argv[++i];
        } else if (arg.rfind("--session-key=", 0) == 0) {
            opts.session_key = arg.substr(14);
        } else if (arg == "--separate-profile" || arg == "--no-real-profile") {
            opts.use_real_profile = false;
        } else if (arg == "--cdp") {
            opts.cdp_port = 9222;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { opts.cdp_port = std::stoi(argv[++i]); } catch (...) {}
            }
        } else if (arg.rfind("--cdp=", 0) == 0) {
            try { opts.cdp_port = std::stoi(arg.substr(6)); } catch (...) {}
        } else if (arg == "--user-data-dir") {
            if (i + 1 < argc) opts.user_data_dir = argv[++i];
        } else if (arg.rfind("--user-data-dir=", 0) == 0) {
            opts.user_data_dir = arg.substr(16);
        } else if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--clean-session") {
            opts.clean_session = true;
        } else if (arg == "--list-profiles") {
            opts.list_profiles = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else if (arg == "--save-session-key" || arg == "session-key") {
            if (i + 1 < argc) {
                opts.save_session_key_arg = argv[++i];
            } else {
                opts.save_session_key_arg = "interactive";
            }
        } else if (arg == "--chrome-debug" || arg == "chrome:debug") {
            opts.launch_chrome_debug = true;
        }
    }

    return opts;
}

void show_help(const char* prog) {
    std::cout << "\nClaude Active Sessions Terminator Bot (C++ Edition)\n"
              << "===================================================\n\n"
              << "Usage:\n"
              << "  " << prog << " [options]\n\n"
              << "Sessions outside India are never terminated. Inside India, these cities are\n"
              << "always protected (cannot be disabled):\n"
              << "  mumbai, navi mumbai, panvel, thane\n\n"
              << "Options:\n"
              << "  --interval, -i <time>     Check interval: seconds, or with a unit like 90s, 7m, 2h\n"
              << "                            (default: 60 = 1 minute bot)\n"
              << "  --once                    Run a single scan and exit (disables recurring bot loop)\n"
              << "  --dry-run                 Preview sessions without actually terminating them\n"
              << "  --allow, -a <locations>   Extra protected Indian cities, comma-separated (case-insensitive).\n"
              << "                            These are ADDED to the always-protected list above.\n"
              << "  --chrome-profile <name>   Local Chrome profile: folder name, display name or account\n"
              << "                            email (default: \"" << DEFAULT_CHROME_PROFILE << "\")\n"
              << "  --chrome-path <exe>       Path to chrome executable (default: auto-detected local install)\n"
              << "  --session-key <value>     Reuse your existing claude.ai session cookie instead of\n"
              << "                            signing in again (also read from .env / session-key.txt)\n"
              << "  --separate-profile        Skip the real Chrome profile; use ./chrome_session only\n"
              << "  --cdp [port]              Attach to a Chrome already running with remote debugging (e.g. 9222)\n"
              << "  --user-data-dir <dir>     Directory to persist browser login data (default: ./chrome_session)\n"
              << "  --headless                Run in headless mode (recommended after initial login is saved)\n"
              << "  --clean-session           Reset session directory\n"
              << "  --list-profiles           List the local Chrome profiles and exit\n"
              << "  --save-session-key <val>  Save cookie to session-key.txt\n"
              << "  --chrome-debug            Start Chrome with remote debugging on profile and exit\n"
              << "  -h, --help                Show this help message\n\n";
}

} // namespace chokidar
