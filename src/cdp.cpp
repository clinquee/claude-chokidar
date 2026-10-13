#include "cdp.hpp"
#include <iostream>

namespace chokidar {

CdpSession::CdpSession() = default;

CdpSession::~CdpSession() {
    disconnect();
}

bool CdpSession::connect(const std::string& ws_url, int timeout_ms) {
    disconnect();
    if (!ws_.connect(ws_url, timeout_ms)) {
        return false;
    }
    // Enable core domains
    send_command("Page.enable");
    send_command("Network.enable");
    send_command("Runtime.enable");
    return true;
}

void CdpSession::disconnect() {
    ws_.close();
    pending_responses_.clear();
}

bool CdpSession::is_connected() const {
    return ws_.is_connected();
}

JsonValue CdpSession::send_command(const std::string& method, const JsonValue& params, int timeout_ms) {
    if (!is_connected()) return JsonValue();

    int64_t msg_id = next_id_++;
    JsonValue msg = JsonValue::object();
    msg["id"] = msg_id;
    msg["method"] = method;
    if (params.is_object()) {
        msg["params"] = params;
    }

    std::string serialized = msg.dump();
    if (!ws_.send_text(serialized)) {
        return JsonValue();
    }

    // Check if response already buffered
    auto it = pending_responses_.find(msg_id);
    if (it != pending_responses_.end()) {
        JsonValue resp = it->second;
        pending_responses_.erase(it);
        return resp;
    }

    int64_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        std::string raw;
        int remaining_timeout = static_cast<int>(timeout_ms - (now_ms() - start));
        if (remaining_timeout <= 0) break;

        if (!ws_.receive_text(raw, remaining_timeout)) {
            break;
        }

        JsonValue parsed = JsonValue::parse(raw);
        if (parsed.contains("id")) {
            int64_t recv_id = parsed["id"].as_int();
            if (recv_id == msg_id) {
                return parsed;
            } else {
                pending_responses_[recv_id] = parsed;
            }
        }
        // If message has no id, it's a CDP event, discard or handle
    }

    return JsonValue();
}

JsonValue CdpSession::evaluate(const std::string& expression, int timeout_ms) {
    JsonValue params = JsonValue::object();
    params["expression"] = expression;
    params["awaitPromise"] = true;
    params["returnByValue"] = true;

    JsonValue resp = send_command("Runtime.evaluate", params, timeout_ms);
    if (resp.contains("result")) {
        const auto& r = resp["result"];
        if (r.contains("result")) {
            const auto& inner = r["result"];
            if (inner.contains("value")) {
                return inner["value"];
            }
            return inner;
        }
        return r;
    }
    return JsonValue();
}

bool CdpSession::navigate(const std::string& url, int timeout_ms) {
    JsonValue params = JsonValue::object();
    params["url"] = url;
    JsonValue resp = send_command("Page.navigate", params, timeout_ms);
    return resp.contains("result");
}

bool CdpSession::set_cookies(const std::vector<SessionCookie>& cookies) {
    if (cookies.empty()) return true;

    // Set cookie expiration 1 year out
    int64_t expires = (now_ms() / 1000) + 365 * 24 * 3600;

    std::map<std::string, std::vector<std::string>> by_name;
    for (const auto& c : cookies) {
        by_name[c.name].push_back(c.value);
    }

    bool all_ok = true;
    for (const auto& kv : by_name) {
        const std::string& name = kv.first;
        const auto& values = kv.second;

        // claude.ai stores one host-only and one domain-wide copy
        std::vector<std::string> domains = {".claude.ai", "claude.ai"};
        for (size_t i = 0; i < values.size() && i < 2; ++i) {
            std::vector<std::string> target_domains = (values.size() > 1) ?
                std::vector<std::string>{domains[i]} : domains;

            for (const auto& domain : target_domains) {
                JsonValue params = JsonValue::object();
                params["name"] = name;
                params["value"] = values[i];
                params["domain"] = domain;
                params["path"] = "/";
                params["httpOnly"] = true;
                params["secure"] = true;
                params["sameSite"] = "Lax";
                params["expires"] = expires;

                JsonValue resp = send_command("Network.setCookie", params);
                if (resp.contains("result")) {
                    if (resp["result"]["success"].is_bool() && !resp["result"]["success"].as_bool()) {
                        all_ok = false;
                    }
                }
            }
        }
    }
    return all_ok;
}

bool CdpSession::add_init_script(const std::string& script) {
    JsonValue params = JsonValue::object();
    params["source"] = script;
    JsonValue resp = send_command("Page.addScriptToEvaluateOnNewDocument", params);
    return resp.contains("result");
}

bool CdpSession::bring_to_front() {
    JsonValue resp = send_command("Page.bringToFront");
    return resp.contains("result");
}

} // namespace chokidar
