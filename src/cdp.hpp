#pragma once

#include "net.hpp"
#include "json.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <map>

namespace chokidar {

class CdpSession {
public:
    CdpSession();
    ~CdpSession();

    bool connect(const std::string& ws_url, int timeout_ms = 8000);
    void disconnect();
    bool is_connected() const;

    // Send generic CDP command and wait for response
    JsonValue send_command(const std::string& method, const JsonValue& params = JsonValue::object(), int timeout_ms = 15000);

    // Evaluate JavaScript in page context
    JsonValue evaluate(const std::string& expression, int timeout_ms = 15000);

    // Navigate to a URL
    bool navigate(const std::string& url, int timeout_ms = 30000);

    // Inject session cookies into the browser context
    bool set_cookies(const std::vector<SessionCookie>& cookies);

    // Inject script that runs before any document loads
    bool add_init_script(const std::string& script);

    // Bring tab to front
    bool bring_to_front();

private:
    WebSocketClient ws_;
    std::atomic<int64_t> next_id_{1};
    std::map<int64_t, JsonValue> pending_responses_;
};

} // namespace chokidar
