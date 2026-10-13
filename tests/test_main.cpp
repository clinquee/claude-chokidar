#include "../src/safety.hpp"
#include "../src/config.hpp"
#include "../src/json.hpp"
#include "../src/common.hpp"
#include <iostream>
#include <cassert>

using namespace chokidar;

void test_india_detection() {
    std::cout << "[TEST] Running India location detection tests...\n";
    assert(is_in_india("Mumbai, Maharashtra, IN") == true);
    assert(is_in_india("pune, maharashtra, in") == true);
    assert(is_in_india("Pune, Maharashtra, India") == true);
    assert(is_in_india("Bergen, Vestland, NO") == false);
    assert(is_in_india("London, England, GB") == false);
    assert(is_in_india("Indianapolis, Indiana, US") == false);
    assert(is_in_india("IN") == false);
    assert(is_in_india("") == false);
    assert(is_in_india("India") == false);
    std::cout << "  ✓ India detection passed\n";
}

void test_location_allowed() {
    std::cout << "[TEST] Running location allowance tests...\n";
    std::vector<std::string> empty_extras;
    // Outside India is always allowed
    assert(is_location_allowed("Bergen, Vestland, NO", empty_extras) == true);
    assert(is_location_allowed("London, England, GB", empty_extras) == true);

    // Built-in protected locations in India
    assert(is_location_allowed("Mumbai, Maharashtra, IN", empty_extras) == true);
    assert(is_location_allowed("Navi Mumbai, Maharashtra, IN", empty_extras) == true);
    assert(is_location_allowed("Panvel, Maharashtra, IN", empty_extras) == true);
    assert(is_location_allowed("Thane, Maharashtra, IN", empty_extras) == true);

    // Unprotected cities in India
    assert(is_location_allowed("Pune, Maharashtra, IN", empty_extras) == false);
    assert(is_location_allowed("Nashik, Maharashtra, IN", empty_extras) == false);
    assert(is_location_allowed("Delhi, IN", empty_extras) == false);

    // Allowed extras
    std::vector<std::string> extras = {"pune", "nashik"};
    assert(is_location_allowed("Pune, Maharashtra, IN", extras) == true);
    assert(is_location_allowed("Nashik, Maharashtra, IN", extras) == true);
    assert(is_location_allowed("Delhi, IN", extras) == false);
    std::cout << "  ✓ Location allowance passed\n";
}

void test_session_classification() {
    std::cout << "[TEST] Running session classification tests...\n";
    std::vector<std::string> empty_extras;

    // Current session is always kept
    SessionRow current;
    current.device_text = "Chrome (Mac)";
    current.location_text = "Pune, Maharashtra, IN";
    current.is_current = true;
    auto v_cur = classify_session(current, empty_extras);
    assert(v_cur.keep == true);
    assert(v_cur.reason == "Current session");

    // Unknown location is kept for safety
    SessionRow unk;
    unk.device_text = "Chrome (Linux)";
    unk.location_text = "-";
    auto v_unk = classify_session(unk, empty_extras);
    assert(v_unk.keep == true);
    assert(v_unk.reason == "Location unknown - kept for safety");

    // Outside India
    SessionRow abroad;
    abroad.device_text = "Claude (iOS)";
    abroad.location_text = "Bergen, Vestland, NO";
    auto v_abroad = classify_session(abroad, empty_extras);
    assert(v_abroad.keep == true);
    assert(v_abroad.reason == "Outside India (or country not shown)");

    // Protected in India
    SessionRow mum;
    mum.device_text = "Chrome (Windows)";
    mum.location_text = "Mumbai, Maharashtra, IN";
    auto v_mum = classify_session(mum, empty_extras);
    assert(v_mum.keep == true);
    assert(v_mum.reason == "Protected location");

    // Unprotected in India -> must flag for termination
    SessionRow pune;
    pune.device_text = "Chrome (Linux)";
    pune.location_text = "Pune, Maharashtra, IN";
    auto v_pune = classify_session(pune, empty_extras);
    assert(v_pune.keep == false);
    assert(v_pune.reason.find("not protected") != std::string::npos);

    // Empty device text -> kept for safety
    SessionRow empty_dev;
    empty_dev.device_text = "   ";
    empty_dev.location_text = "Pune, Maharashtra, IN";
    auto v_empty_dev = classify_session(empty_dev, empty_extras);
    assert(v_empty_dev.keep == true);
    assert(v_empty_dev.reason == "Row not readable - kept for safety");

    std::cout << "  ✓ Session classification passed\n";
}

void test_interval_parsing() {
    std::cout << "[TEST] Running interval parsing tests...\n";
    assert(parse_interval("60") == 60);
    assert(parse_interval("300") == 300);
    assert(parse_interval("90s") == 90);
    assert(parse_interval("7m") == 420);
    assert(parse_interval("2h") == 7200);
    assert(parse_interval("0") == -1);
    assert(parse_interval("-10") == -1);
    assert(parse_interval("invalid") == -1);
    std::cout << "  ✓ Interval parsing passed\n";
}

void test_cookie_parsing() {
    std::cout << "[TEST] Running cookie parsing tests...\n";
    // Bare value
    auto c1 = parse_session_cookies("sk-ant-sid02-abcdef123456");
    assert(c1.size() == 1);
    assert(c1[0].name == "sessionKey");
    assert(c1[0].value == "sk-ant-sid02-abcdef123456");

    // Cookie header
    auto c2 = parse_session_cookies("sessionKey=sk-1; sessionKeyV3=sk-2; other=ignore");
    assert(c2.size() == 2);

    // Multi-line
    auto c3 = parse_session_cookies("sessionKey=sk-ant-1\nsessionKeyLC=sk-ant-2\n# comment\n");
    assert(c3.size() == 2);
    std::cout << "  ✓ Cookie parsing passed\n";
}

void test_json_parser() {
    std::cout << "[TEST] Running JSON parser tests...\n";
    std::string sample = R"({
        "status": "ok",
        "count": 42,
        "active": true,
        "items": ["apple", "banana"],
        "nested": {
            "key": "value with \"quotes\" and \n newline"
        }
    })";
    std::string err;
    JsonValue root = JsonValue::parse(sample, &err);
    assert(err.empty());
    assert(root["status"].as_string() == "ok");
    assert(root["count"].as_int() == 42);
    assert(root["active"].as_bool() == true);
    assert(root["items"].size() == 2);
    assert(root["items"][0].as_string() == "apple");
    assert(root["items"][1].as_string() == "banana");
    assert(root["nested"]["key"].as_string().find("quotes") != std::string::npos);

    // Serialize and parse back
    std::string dumped = root.dump();
    JsonValue root2 = JsonValue::parse(dumped, &err);
    assert(err.empty());
    assert(root2["count"].as_int() == 42);
    assert(root2["status"].as_string() == "ok");
    std::cout << "  ✓ JSON parser passed\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << " Running claude-chokidar C++ Test Suite \n";
    std::cout << "========================================\n";
    test_india_detection();
    test_location_allowed();
    test_session_classification();
    test_interval_parsing();
    test_cookie_parsing();
    test_json_parser();
    std::cout << "\nALL TESTS PASSED SUCCESSFULLY! (100% OK)\n";
    return 0;
}
