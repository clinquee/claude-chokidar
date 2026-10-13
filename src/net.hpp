#pragma once

#include <string>
#include <vector>
#include <memory>

namespace chokidar {

struct HttpResponse {
    int status = 0;
    std::string body;
};

// Check if a local port is listening (e.g. Chrome Remote Debugging)
bool is_port_open(int port, int timeout_ms = 1000);

// Perform a simple HTTP GET request to localhost
HttpResponse http_get(int port, const std::string& path, int timeout_ms = 3000);

// Perform a simple HTTP PUT request to localhost
HttpResponse http_put(int port, const std::string& path, int timeout_ms = 3000);

// Low-latency RFC 6455 WebSocket client over POSIX sockets
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    // Connect to ws://host:port/path
    bool connect(const std::string& ws_url, int timeout_ms = 5000);

    // Send a UTF-8 text message (masked per RFC 6455)
    bool send_text(const std::string& text);

    // Receive the next text message (handles pings automatically)
    bool receive_text(std::string& out_text, int timeout_ms = 10000);

    // Close connection
    void close();

    bool is_connected() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    std::vector<uint8_t> read_buffer_;

    bool read_exact(uint8_t* dest, size_t len, int timeout_ms);
    bool write_exact(const uint8_t* src, size_t len, int timeout_ms);
    void send_pong(const std::vector<uint8_t>& payload);
};

} // namespace chokidar
