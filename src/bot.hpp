#pragma once

#include "config.hpp"
#include "safety.hpp"
#include "cdp.hpp"
#include "health_server.hpp"
#include <string>
#include <memory>
#include <atomic>

namespace chokidar {

struct ScanResult {
    int scanned = 0;
    int kept = 0;
    int terminated = 0;
};

class Bot {
public:
    Bot(const Options& options);
    ~Bot();

    int run();
    void request_shutdown();

private:
    Options options_;
    std::atomic<bool> is_shutting_down_{false};
    BotStats stats_;
    std::unique_ptr<HealthServer> health_server_;
    std::unique_ptr<CdpSession> cdp_;
    int chrome_pid_ = -1;
    bool is_cdp_attached_ = false;

    bool open_browser();
    bool check_authentication();
    ScanResult perform_scan(int check_number);
    void cleanup();
    bool arm_logout_guard();
};

} // namespace chokidar
