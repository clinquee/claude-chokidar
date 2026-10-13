#pragma once

#include "common.hpp"
#include <string>
#include <vector>

namespace chokidar {

struct SessionVerdict {
    bool keep = true;
    std::string reason;
};

struct SessionRow {
    std::string device_text;
    std::string location_text;
    std::string created_text;
    bool is_current = false;
    std::string row_id; // internal ID/index for DOM targeting
};

// True only when the location's last comma-separated part is India's country
// code/name AND a city precedes it. Anything else - another country, no country,
// a bare "IN" - is not treated as India, so it is never terminated.
bool is_in_india(const std::string& location);

// Check if a location string is allowed: everything outside India is, and inside
// India only the protected cities (plus any --allow extras) are.
bool is_location_allowed(const std::string& location, const std::vector<std::string>& allowed_locations);

// Decide what to do with one session row - deliberately biased towards keeping.
SessionVerdict classify_session(const SessionRow& session, const std::vector<std::string>& allowed_locations);

} // namespace chokidar
