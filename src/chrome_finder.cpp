#include "chrome_finder.hpp"
#include "json.hpp"
#include "common.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pwd.h>
#endif

namespace chokidar {

static bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::string find_chrome_executable(const std::string& explicit_path) {
    if (!explicit_path.empty() && file_exists(explicit_path)) {
        return explicit_path;
    }

    const char* env_path = std::getenv("CHROME_PATH");
    if (env_path && file_exists(env_path)) {
        return env_path;
    }

    std::vector<std::string> candidates;
#if defined(__APPLE__)
    candidates.push_back("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    candidates.push_back("/Applications/Chrome.app/Contents/MacOS/Google Chrome");
    const char* home = std::getenv("HOME");
    if (home) {
        candidates.push_back(std::string(home) + "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    }
#elif defined(_WIN32)
    const char* prog_files = std::getenv("PROGRAMFILES");
    const char* prog_files_x86 = std::getenv("PROGRAMFILES(X86)");
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (prog_files) candidates.push_back(std::string(prog_files) + "\\Google\\Chrome\\Application\\chrome.exe");
    if (prog_files_x86) candidates.push_back(std::string(prog_files_x86) + "\\Google\\Chrome\\Application\\chrome.exe");
    if (local_app_data) candidates.push_back(std::string(local_app_data) + "\\Google\\Chrome\\Application\\chrome.exe");
#else
    candidates.push_back("/usr/bin/google-chrome");
    candidates.push_back("/usr/bin/google-chrome-stable");
    candidates.push_back("/opt/google/chrome/chrome");
    candidates.push_back("/usr/bin/chromium");
    candidates.push_back("/usr/bin/chromium-browser");
#endif

    for (const auto& path : candidates) {
        if (file_exists(path)) return path;
    }
    return "";
}

std::string default_user_data_dir() {
#if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        return std::string(home) + "/Library/Application Support/Google/Chrome";
    }
    return "";
#elif defined(_WIN32)
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data) {
        return std::string(local_app_data) + "\\Google\\Chrome\\User Data";
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/google-chrome";
    }
    return "";
#endif
}

std::vector<ChromeProfileInfo> list_chrome_profiles(const std::string& user_data_dir) {
    std::string dir = user_data_dir.empty() ? default_user_data_dir() : user_data_dir;
    std::string local_state_path = dir + "/Local State";

    std::ifstream file(local_state_path);
    if (!file.is_open()) return {};

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    std::string err;
    JsonValue root = JsonValue::parse(content, &err);
    if (root.is_null()) return {};

    std::string last_used = root["profile"]["last_used"].as_string();
    const auto& info_cache = root["profile"]["info_cache"];
    if (!info_cache.is_object()) return {};

    std::vector<ChromeProfileInfo> profiles;
    for (const auto& kv : info_cache.obj_val) {
        ChromeProfileInfo p;
        p.dir = kv.first;
        p.name = kv.second["name"].as_string();
        if (p.name.empty()) p.name = p.dir;
        p.email = kv.second["user_name"].as_string();
        p.is_last_used = (p.dir == last_used);
        std::string lock_path = dir + "/" + p.dir + "/lockfile";
        p.is_locked = file_exists(lock_path);
        profiles.push_back(p);
    }
    return profiles;
}

ChromeProfileInfo resolve_chrome_profile(const std::string& user_data_dir, const std::string& hint) {
    auto profiles = list_chrome_profiles(user_data_dir);
    if (profiles.empty()) return {};

    if (hint.empty()) return profiles[0];

    std::string needle = to_lower(trim(hint));
    // Exact match first
    for (const auto& p : profiles) {
        if (to_lower(p.dir) == needle || to_lower(p.name) == needle || to_lower(p.email) == needle) {
            return p;
        }
    }
    // Substring match
    for (const auto& p : profiles) {
        if (to_lower(p.dir).find(needle) != std::string::npos ||
            to_lower(p.name).find(needle) != std::string::npos ||
            to_lower(p.email).find(needle) != std::string::npos) {
            return p;
        }
    }
    return {};
}

bool is_chrome_running() {
#if defined(__APPLE__) || defined(__linux__)
    FILE* fp = popen("pgrep -i 'chrome' 2>/dev/null", "r");
    if (!fp) return false;
    char buf[128];
    bool running = (fgets(buf, sizeof(buf), fp) != nullptr);
    pclose(fp);
    return running;
#elif defined(_WIN32)
    // Simple check on windows using tasklist
    FILE* fp = _popen("tasklist /FI \"IMAGENAME eq chrome.exe\" 2>NUL", "r");
    if (!fp) return false;
    char buf[256];
    bool running = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "chrome.exe")) {
            running = true;
            break;
        }
    }
    _pclose(fp);
    return running;
#else
    return false;
#endif
}

} // namespace chokidar
