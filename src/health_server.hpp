#pragma once

#include <string>
#include <atomic>
#include <thread>

namespace chokidar {

struct BotStats {
    std::atomic<int> scans{0};
    std::atomic<int> kept{0};
    std::atomic<int> terminated{0};
    int64_t started_at = 0;
};

class HealthServer {
public:
    HealthServer(int port, BotStats* stats);
    ~HealthServer();

    bool start();
    void stop();

private:
    int port_ = 0;
    BotStats* stats_ = nullptr;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    void run();
};

} // namespace chokidar
