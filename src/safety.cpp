#include "safety.hpp"
#include <set>

namespace chokidar {

bool is_in_india(const std::string& location) {
    auto parts = split(location, ',');
    std::vector<std::string> clean_parts;
    for (auto& p : parts) {
        std::string s = to_lower(trim(p));
        if (!s.empty()) {
            clean_parts.push_back(s);
        }
    }
    if (clean_parts.size() < 2) {
        return false;
    }
    const std::string& last = clean_parts.back();
    for (const auto& india_name : INDIA_COUNTRY_NAMES) {
        if (last == india_name) return true;
    }
    return false;
}

bool is_location_allowed(const std::string& location, const std::vector<std::string>& allowed_locations) {
    if (location.empty()) return false;
    if (!is_in_india(location)) return true; // outside India is always allowed

    std::string loc_lower = to_lower(location);

    std::set<std::string> combined;
    for (const auto& p : PROTECTED_LOCATIONS) {
        combined.insert(to_lower(trim(p)));
    }
    for (const auto& a : allowed_locations) {
        std::string s = to_lower(trim(a));
        if (!s.empty()) combined.insert(s);
    }

    for (const auto& allowed : combined) {
        if (loc_lower.find(allowed) != std::string::npos) {
            return true;
        }
    }
    return false;
}

SessionVerdict classify_session(const SessionRow& session, const std::vector<std::string>& allowed_locations) {
    if (session.is_current) {
        return {true, "Current session"};
    }

    std::string location = collapse_whitespace(session.location_text);

    if (location.empty() || location == "-" || location == "--") {
        return {true, "Location unknown - kept for safety"};
    }

    if (!is_in_india(location)) {
        return {true, "Outside India (or country not shown)"};
    }

    if (is_location_allowed(location, allowed_locations)) {
        return {true, "Protected location"};
    }

    if (trim(session.device_text).empty()) {
        return {true, "Row not readable - kept for safety"};
    }

    return {false, "Location \"" + location + "\" is not protected"};
}

} // namespace chokidar
