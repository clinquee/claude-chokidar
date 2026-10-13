#include "health_server.hpp"
#include "common.hpp"
#include "json.hpp"
#include <iostream>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace chokidar {

HealthServer::HealthServer(int port, BotStats* stats)
    : port_(port), stats_(stats) {}

HealthServer::~HealthServer() {
    stop();
}

bool HealthServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 10) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    worker_thread_ = std::thread(&HealthServer::run, this);
    std::cout << "Cloud keep-alive health check server listening on port " << port_ << std::endl;
    return true;
}

void HealthServer::stop() {
    if (running_.exchange(false)) {
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

void HealthServer::run() {
    while (running_) {
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0 || !running_) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        // Drain request with bounded timeout
        pollfd client_pfd{};
        client_pfd.fd = client_fd;
        client_pfd.events = POLLIN;
        if (poll(&client_pfd, 1, 2000) > 0) {
            char buf[1024];
            recv(client_fd, buf, sizeof(buf) - 1, 0);
        }

        int64_t uptime_sec = stats_ ? ((now_ms() - stats_->started_at) / 1000) : 0;
        JsonValue resp = JsonValue::object();
        resp["status"] = "ok";
        resp["uptimeSeconds"] = uptime_sec;
        resp["scans"] = stats_ ? stats_->scans.load() : 0;
        resp["kept"] = stats_ ? stats_->kept.load() : 0;
        resp["terminated"] = stats_ ? stats_->terminated.load() : 0;

        std::string body = resp.dump();
        std::string http_resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;

        send(client_fd, http_resp.data(), http_resp.size(), 0);
        close(client_fd);
    }
}

} // namespace chokidar
