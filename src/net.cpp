#include "net.hpp"
#include "common.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <random>

namespace chokidar {

static int set_nonblocking(int fd, bool nonblock) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

static int connect_with_timeout(const std::string& host, int port, int timeout_ms) {
#if defined(SIGPIPE)
    static bool sigpipe_ignored = false;
    if (!sigpipe_ignored) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = true;
    }
#endif

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Enable TCP_NODELAY for ultra-low latency
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char*)&flag, sizeof(flag));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        // Fallback for localhost
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    set_nonblocking(fd, true);
    int res = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (res != 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) {
            close(fd);
            return -1;
        }

        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }

    set_nonblocking(fd, false);
    return fd;
}

bool is_port_open(int port, int timeout_ms) {
    int fd = connect_with_timeout("127.0.0.1", port, timeout_ms);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

static HttpResponse parse_http_response(int fd, int timeout_ms) {
    HttpResponse resp;
    std::string raw;
    char buf[4096];

    while (true) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) break;

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);

        size_t header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            // Check Content-Length if present
            size_t cl_pos = raw.find("Content-Length:");
            if (cl_pos == std::string::npos) cl_pos = raw.find("content-length:");
            if (cl_pos != std::string::npos && cl_pos < header_end) {
                size_t line_end = raw.find("\r\n", cl_pos);
                std::string cl_str = trim(raw.substr(cl_pos + 15, line_end - (cl_pos + 15)));
                try {
                    size_t expected_len = std::stoul(cl_str);
                    if (raw.size() >= header_end + 4 + expected_len) {
                        break;
                    }
                } catch (...) {}
            }
        }
    }

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        std::string headers = raw.substr(0, header_end);
        resp.body = raw.substr(header_end + 4);

        size_t first_line_end = headers.find("\r\n");
        if (first_line_end != std::string::npos) {
            std::string status_line = headers.substr(0, first_line_end);
            auto parts = split(status_line, ' ');
            if (parts.size() >= 2) {
                try { resp.status = std::stoi(parts[1]); } catch (...) {}
            }
        }
    } else {
        resp.body = raw;
    }

    return resp;
}

HttpResponse http_get(int port, const std::string& path, int timeout_ms) {
    int fd = connect_with_timeout("127.0.0.1", port, timeout_ms);
    if (fd < 0) return {0, ""};

    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                      "User-Agent: claude-chokidar-cpp\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n\r\n";

    send(fd, req.data(), req.size(), 0);
    auto resp = parse_http_response(fd, timeout_ms);
    close(fd);
    return resp;
}

HttpResponse http_put(int port, const std::string& path, int timeout_ms) {
    int fd = connect_with_timeout("127.0.0.1", port, timeout_ms);
    if (fd < 0) return {0, ""};

    std::string req = "PUT " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
                      "User-Agent: claude-chokidar-cpp\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n\r\n";

    send(fd, req.data(), req.size(), 0);
    auto resp = parse_http_response(fd, timeout_ms);
    close(fd);
    return resp;
}

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    close();
}

void WebSocketClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    read_buffer_.clear();
}

bool WebSocketClient::read_exact(uint8_t* dest, size_t len, int timeout_ms) {
    size_t total = 0;
    while (total < len) {
        if (!read_buffer_.empty()) {
            size_t take = std::min(len - total, read_buffer_.size());
            std::memcpy(dest + total, read_buffer_.data(), take);
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + take);
            total += take;
            continue;
        }

        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return false;

        uint8_t buf[8192];
        ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) return false;

        size_t take = std::min(len - total, (size_t)n);
        std::memcpy(dest + total, buf, take);
        total += take;

        if ((size_t)n > take) {
            read_buffer_.insert(read_buffer_.end(), buf + take, buf + n);
        }
    }
    return true;
}

bool WebSocketClient::write_exact(const uint8_t* src, size_t len, int timeout_ms) {
    size_t total = 0;
    while (total < len) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return false;

        ssize_t n = send(fd_, src + total, len - total, 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

bool WebSocketClient::connect(const std::string& ws_url, int timeout_ms) {
    close();

    // URL shape: ws://127.0.0.1:9222/devtools/page/...
    std::string prefix = "ws://";
    if (ws_url.rfind(prefix, 0) != 0) return false;

    std::string rest = ws_url.substr(prefix.size());
    size_t slash_pos = rest.find('/');
    std::string host_port = (slash_pos == std::string::npos) ? rest : rest.substr(0, slash_pos);
    std::string path = (slash_pos == std::string::npos) ? "/" : rest.substr(slash_pos);

    std::string host = "127.0.0.1";
    int port = 9222;
    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        try { port = std::stoi(host_port.substr(colon_pos + 1)); } catch (...) {}
    } else {
        host = host_port;
    }

    fd_ = connect_with_timeout(host, port, timeout_ms);
    if (fd_ < 0) return false;

    // Send WebSocket Upgrade Handshake
    std::string handshake =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host_port + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    if (!write_exact((const uint8_t*)handshake.data(), handshake.size(), timeout_ms)) {
        close();
        return false;
    }

    // Read Handshake response until \r\n\r\n
    std::string response;
    char c = 0;
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (!read_exact((uint8_t*)&c, 1, timeout_ms)) {
            close();
            return false;
        }
        response += c;
    }

    if (response.find("101 Switching Protocols") == std::string::npos &&
        response.find("101 switching protocols") == std::string::npos &&
        response.find("101") == std::string::npos) {
        close();
        return false;
    }

    return true;
}

bool WebSocketClient::send_text(const std::string& text) {
    if (fd_ < 0) return false;

    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN = 1, opcode = 1 (text)

    size_t len = text.size();
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }

    // 4-byte mask key
    static std::mt19937 rng(std::random_device{}());
    uint8_t mask[4] = {
        static_cast<uint8_t>(rng() & 0xFF),
        static_cast<uint8_t>(rng() & 0xFF),
        static_cast<uint8_t>(rng() & 0xFF),
        static_cast<uint8_t>(rng() & 0xFF)
    };
    for (int i = 0; i < 4; ++i) frame.push_back(mask[i]);

    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
    }

    return write_exact(frame.data(), frame.size(), 5000);
}

void WebSocketClient::send_pong(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x8A); // FIN=1, opcode=0xA (pong)
    size_t len = payload.size();
    frame.push_back(0x80 | static_cast<uint8_t>(len));
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; ++i) frame.push_back(mask[i]);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }
    write_exact(frame.data(), frame.size(), 1000);
}

bool WebSocketClient::receive_text(std::string& out_text, int timeout_ms) {
    if (fd_ < 0) return false;

    while (true) {
        uint8_t header[2];
        if (!read_exact(header, 2, timeout_ms)) return false;

        uint8_t opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            uint8_t ext[2];
            if (!read_exact(ext, 2, timeout_ms)) return false;
            payload_len = (ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (!read_exact(ext, 8, timeout_ms)) return false;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | ext[i];
            }
        }

        // Bounded frame check: reject > 32MB to prevent unbounded allocation
        if (payload_len > 32 * 1024 * 1024) {
            close();
            return false;
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (!read_exact(mask, 4, timeout_ms)) return false;
        }

        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0) {
            if (!read_exact(payload.data(), payload_len, timeout_ms)) return false;
            if (masked) {
                for (size_t i = 0; i < payload_len; ++i) {
                    payload[i] ^= mask[i % 4];
                }
            }
        }

        if (opcode == 0x9) { // Ping
            send_pong(payload);
            continue;
        } else if (opcode == 0x8) { // Close
            close();
            return false;
        } else if (opcode == 0x1) { // Text
            out_text.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
            return true;
        }
        // Other opcodes ignored, loop for next
    }
}

} // namespace chokidar
