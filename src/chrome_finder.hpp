#pragma once

#include <string>
#include <vector>

namespace chokidar {

struct ChromeProfileInfo {
    std::string dir;
    std::string name;
    std::string email;
    bool is_locked = false;
    bool is_last_used = false;
};

// Locate the user's installed Chrome binary
std::string find_chrome_executable(const std::string& explicit_path = "");

// Default location of the live Chrome user-data-dir for this OS user
std::string default_user_data_dir();

// Read the profile list Chrome keeps in "Local State"
std::vector<ChromeProfileInfo> list_chrome_profiles(const std::string& user_data_dir = "");

// Match a profile by folder name, display name or account email
ChromeProfileInfo resolve_chrome_profile(const std::string& user_data_dir, const std::string& hint);

// Check if Chrome is currently running
bool is_chrome_running();

} // namespace chokidar
