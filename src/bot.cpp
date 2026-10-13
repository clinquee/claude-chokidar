#include "bot.hpp"
#include "chrome_finder.hpp"
#include "json.hpp"
#include "common.hpp"
#include <iostream>
#include <set>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

namespace chokidar {

static Bot* g_bot_instance = nullptr;

static void signal_handler(int sig) {
    (void)sig;
    if (g_bot_instance) {
        g_bot_instance->request_shutdown();
    }
}

Bot::Bot(const Options& options)
    : options_(options) {
    stats_.started_at = options_.bot_started_at;
    g_bot_instance = this;
}

Bot::~Bot() {
    cleanup();
    if (g_bot_instance == this) {
        g_bot_instance = nullptr;
    }
}

void Bot::request_shutdown() {
    is_shutting_down_ = true;
}

void Bot::cleanup() {
    if (health_server_) {
        health_server_->stop();
        health_server_.reset();
    }
    if (cdp_) {
        cdp_->disconnect();
        cdp_.reset();
    }
    if (chrome_pid_ > 0 && !is_cdp_attached_) {
        kill(chrome_pid_, SIGTERM);
        int status;
        waitpid(chrome_pid_, &status, WNOHANG);
        chrome_pid_ = -1;
    }
}

bool Bot::arm_logout_guard() {
    if (!cdp_ || !cdp_->is_connected()) return false;
    const char* guard_script = R"(
        (() => {
            if (window.__claudeBotLogoutGuard) return;
            window.__claudeBotLogoutGuard = true;
            const FORBIDDEN = /log\s*out|sign\s*out/i;
            const block = event => {
                const el = event.target && event.target.closest
                    ? event.target.closest('button, a, [role="menuitem"], [role="button"]')
                    : null;
                if (!el) return;
                const label = (el.innerText || el.textContent || '') + ' ' + (el.getAttribute('aria-label') || '');
                if (FORBIDDEN.test(label)) {
                    event.preventDefault();
                    event.stopImmediatePropagation();
                    console.warn('[claude-bot] Blocked click on:', label.trim());
                }
            };
            for (const type of ['pointerdown', 'mousedown', 'click']) {
                document.addEventListener(type, block, true);
            }
        })()
    )";
    cdp_->add_init_script(guard_script);
    cdp_->evaluate(guard_script);
    return true;
}

bool Bot::open_browser() {
    int target_port = options_.cdp_port > 0 ? options_.cdp_port : BOT_PROFILE_DEBUG_PORT;

    // 1. If CDP port is already open, attach directly
    if (is_port_open(target_port, 500)) {
        std::cout << "\nAttaching to your running Chrome over CDP (http://127.0.0.1:" << target_port << ")..." << std::endl;
        is_cdp_attached_ = true;
    } else {
        // 2. Locate Chrome executable
        std::string chrome_exe = find_chrome_executable(options_.chrome_path);
        if (chrome_exe.empty()) {
            std::cerr << "\nCould not find a local Chrome install. Set --chrome-path or CHROME_PATH.\n";
            return false;
        }
        std::cout << "\nUsing your local Chrome: " << chrome_exe << std::endl;

        // Try launching real profile if permitted
        std::string user_data = options_.user_data_dir;
        std::string default_user_data = default_user_data_dir();
        auto profile = resolve_chrome_profile(default_user_data, options_.chrome_profile);
        if (profile.dir.empty()) profile.dir = "Default";

        std::cout << "Chrome account    : " << (profile.email.empty() ? profile.name : profile.email)
                  << " (" << profile.dir << ")" << std::endl;

        if (options_.clean_session) {
            std::string rm_cmd = "rm -rf \"" + user_data + "\"";
            system(rm_cmd.c_str());
        }

        mkdir(user_data.c_str(), 0755);
        unlink((user_data + "/SingletonLock").c_str());
        unlink((user_data + "/SingletonCookie").c_str());
        unlink((user_data + "/SingletonSocket").c_str());

        // Spawn Chrome
        std::vector<std::string> args = {
            chrome_exe,
            "--remote-debugging-port=" + std::to_string(target_port),
            "--user-data-dir=" + user_data,
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-blink-features=AutomationControlled"
        };

        if (options_.headless) {
            args.push_back("--headless=new");
            args.push_back("--disable-gpu");
            args.push_back("--no-sandbox");
            args.push_back("--disable-dev-shm-usage");
            args.push_back("--disable-extensions");
            args.push_back("--user-agent=Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36");
        }

        args.push_back(SETTINGS_URL);

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            std::vector<char*> c_args;
            for (auto& a : args) c_args.push_back(const_cast<char*>(a.c_str()));
            c_args.push_back(nullptr);

            // Redirect stdout and stderr to /dev/null
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);

            execvp(c_args[0], c_args.data());
            _exit(1);
        } else if (pid > 0) {
            chrome_pid_ = pid;
        } else {
            std::cerr << "Failed to fork Chrome process.\n";
            return false;
        }

        // Wait for port to open
        int64_t start_wait = now_ms();
        bool ready = false;
        while (now_ms() - start_wait < 20000) {
            if (is_port_open(target_port, 250)) {
                ready = true;
                break;
            }
            sleep_ms(250);
        }

        if (!ready) {
            std::cerr << "Chrome started but remote debugging port " << target_port << " never opened.\n";
            return false;
        }
    }

    // Find tab websocket URL
    auto list_resp = http_get(target_port, "/json/list", 3000);
    std::string ws_url;
    JsonValue tabs = JsonValue::parse(list_resp.body);
    if (tabs.is_array()) {
        for (size_t i = 0; i < tabs.size(); ++i) {
            const auto& tab = tabs[i];
            std::string url = tab["url"].as_string();
            if (url.find("claude.ai") != std::string::npos) {
                ws_url = tab["webSocketDebuggerUrl"].as_string();
                std::cout << "Found an existing Claude tab: " << url << std::endl;
                break;
            }
        }
        if (ws_url.empty() && tabs.size() > 0) {
            ws_url = tabs[0]["webSocketDebuggerUrl"].as_string();
        }
    }

    if (ws_url.empty()) {
        // Open new tab
        auto new_resp = http_put(target_port, "/json/new?" + SETTINGS_URL, 3000);
        JsonValue new_tab = JsonValue::parse(new_resp.body);
        ws_url = new_tab["webSocketDebuggerUrl"].as_string();
    }

    if (ws_url.empty()) {
        std::cerr << "Could not obtain WebSocket debugger URL for tab.\n";
        return false;
    }

    cdp_ = std::make_unique<CdpSession>();
    if (!cdp_->connect(ws_url)) {
        std::cerr << "Failed to connect WebSocket to " << ws_url << "\n";
        return false;
    }

    // Anti-automation evasion
    cdp_->add_init_script(R"(
        try {
            Object.defineProperty(navigator, 'webdriver', { get: () => undefined });
            if (!window.chrome) window.chrome = {};
            if (!window.chrome.runtime) window.chrome.runtime = {};
        } catch (e) {}
    )");

    // Apply session cookies
    auto session_cookies = read_session_cookies(options_.session_key);
    if (!session_cookies.empty()) {
        std::cout << "Reusing your existing claude.ai session (";
        for (size_t i = 0; i < session_cookies.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << session_cookies[i].name;
        }
        std::cout << ") - no new sign-in." << std::endl;
        cdp_->set_cookies(session_cookies);
    }

    arm_logout_guard();
    return true;
}

bool Bot::check_authentication() {
    std::cout << "Navigating to " << SETTINGS_URL << "..." << std::endl;
    cdp_->navigate(SETTINGS_URL);

    std::cout << "Checking authentication status..." << std::endl;

    int max_wait = (options_.headless ? 30000 : 300000);
    int64_t start = now_ms();
    bool prompted = false;

    while (now_ms() - start < max_wait && !is_shutting_down_) {
        // Probe signed-in status
        const char* probe_script = R"(
            (() => {
                const url = window.location.href;
                if (url.includes('/login') || url.includes('/magic-link') || url.includes('/onboarding')) return false;
                if (document.title.includes('Just a moment')) return false;
                const bodyText = document.body ? document.body.innerText : '';
                if (bodyText.includes('Active sessions') || bodyText.includes('Log out of all devices')) return true;
                if (document.querySelector('[data-testid="user-menu-button"]')) return true;
                return false;
            })()
        )";
        JsonValue is_auth = cdp_->evaluate(probe_script);
        if (is_auth.is_bool() && is_auth.as_bool()) {
            return true;
        }

        JsonValue url_val = cdp_->evaluate("window.location.href");
        std::string current_url = url_val.as_string();
        bool on_login = current_url.find("/login") != std::string::npos || current_url.find("/magic-link") != std::string::npos;

        if (on_login && !prompted) {
            prompted = true;
            std::cout << "\n[Action required] This browser profile is not signed in to Claude.\n"
                      << "Sign in once or provide a session key in .env / session-key.txt.\n";
        }

        sleep_ms(2000);
    }

    return false;
}

ScanResult Bot::perform_scan(int check_number) {
    ScanResult res;
    std::string time = get_timestamp();
    std::cout << "\n[" << time << "] Check #" << check_number << ": Inspecting active sessions..." << std::endl;

    cdp_->navigate(SETTINGS_URL);
    sleep_ms(1500);
    arm_logout_guard();

    // Locate Active sessions table
    const char* locate_script = R"(
        (() => {
            const txt = el => (el.innerText || el.textContent || '').trim().replace(/\s+/g, ' ');
            document.querySelectorAll('[data-chokidar-sessions]').forEach(el => el.removeAttribute('data-chokidar-sessions'));
            const heading = [...document.querySelectorAll('h1, h2, h3, h4, h5, h6')].find(el => /^active sessions$/i.test(txt(el)));
            if (!heading) return null;
            const section = heading.closest('section') || heading.parentElement;
            let table = section ? section.querySelector('table') : null;
            if (!table) {
                table = [...document.querySelectorAll('table')].find(t => heading.compareDocumentPosition(t) & Node.DOCUMENT_POSITION_FOLLOWING) || null;
            }
            if (!table) return null;
            const headRow = table.querySelector('thead tr') || table.querySelector('tr');
            if (!headRow) return null;
            const headers = [...headRow.querySelectorAll('th, td')].map(txt);
            const indexOf = name => headers.findIndex(h => new RegExp('^' + name + '$', 'i').test(h));
            table.setAttribute('data-chokidar-sessions', '1');
            return {
                headers: headers,
                columns: {
                    device: indexOf('device'),
                    location: indexOf('location'),
                    created: indexOf('created')
                },
                hasThead: !!table.querySelector('thead')
            };
        })()
    )";

    JsonValue table_info = cdp_->evaluate(locate_script);
    if (!table_info.is_object() || table_info["columns"]["device"].as_int() < 0 || table_info["columns"]["location"].as_int() < 0) {
        // Wait another 3s and retry once
        sleep_ms(3000);
        table_info = cdp_->evaluate(locate_script);
    }

    if (!table_info.is_object()) {
        std::cerr << "[" << time << "] Could not find the \"Active sessions\" section. Re-checking on next interval.\n";
        return res;
    }

    int dev_col = static_cast<int>(table_info["columns"]["device"].as_int());
    int loc_col = static_cast<int>(table_info["columns"]["location"].as_int());
    int crt_col = static_cast<int>(table_info["columns"]["created"].as_int());
    bool has_thead = table_info["hasThead"].as_bool();

    // Script to read all rows
    std::string read_rows_script =
        "((cols, hasThead) => {\n"
        "    const table = document.querySelector('table[data-chokidar-sessions=\"1\"]');\n"
        "    if (!table) return [];\n"
        "    const rows = hasThead ? table.querySelectorAll('tbody tr') : table.querySelectorAll('tr:not(:first-child)');\n"
        "    const out = [];\n"
        "    rows.forEach((row, idx) => {\n"
        "        const cells = row.querySelectorAll('th, td');\n"
        "        if (cells.length === 0) return;\n"
        "        const getCell = i => (i >= 0 && i < cells.length) ? (cells[i].innerText || cells[i].textContent || '').trim() : '';\n"
        "        const deviceText = getCell(cols.device);\n"
        "        const locationText = getCell(cols.location);\n"
        "        const createdText = getCell(cols.created);\n"
        "        if (!deviceText && !locationText) return;\n"
        "        const isCurrent = deviceText.toLowerCase().includes('current') ||\n"
        "                          (row.innerText || '').toLowerCase().includes('current') ||\n"
        "                          row.querySelector('button[aria-label*=\"current session\"]') !== null;\n"
        "        out.push({\n"
        "            index: idx,\n"
        "            deviceText: deviceText,\n"
        "            locationText: locationText,\n"
        "            createdText: createdText,\n"
        "            isCurrent: isCurrent\n"
        "        });\n"
        "    });\n"
        "    return out;\n"
        "})({device: " + std::to_string(dev_col) + ", location: " + std::to_string(loc_col) + ", created: " + std::to_string(crt_col) + "}, " + (has_thead ? "true" : "false") + ")";

    std::set<std::string> processed_keys;
    bool continue_scanning = true;

    while (continue_scanning && !is_shutting_down_) {
        continue_scanning = false;
        JsonValue rows_val = cdp_->evaluate(read_rows_script);
        if (!rows_val.is_array() || rows_val.size() == 0) break;

        std::vector<SessionRow> rows;
        for (size_t i = 0; i < rows_val.size(); ++i) {
            const auto& r = rows_val[i];
            SessionRow row;
            row.device_text = r["deviceText"].as_string();
            row.location_text = r["locationText"].as_string();
            row.created_text = r["createdText"].as_string();
            row.is_current = r["isCurrent"].as_bool();
            row.row_id = std::to_string(r["index"].as_int());
            rows.push_back(row);
        }

        int readable_rows = 0;
        int keepable_rows = 0;
        int target_row_idx = -1;
        SessionRow target_session;

        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            std::string session_key = row.device_text + "_" + row.location_text + "_" + row.created_text;
            auto verdict = classify_session(row, options_.allowed_locations);

            readable_rows++;
            if (verdict.keep) keepable_rows++;

            if (verdict.keep) {
                if (processed_keys.find(session_key) == processed_keys.end()) {
                    std::cout << "  [KEEP]  " << replace_all(row.device_text, "\n", " ")
                              << " | Location: " << (row.location_text.empty() ? "N/A" : row.location_text)
                              << " | " << verdict.reason << std::endl;
                    processed_keys.insert(session_key);
                    res.kept++;
                }
            } else {
                if (target_row_idx < 0) {
                    target_row_idx = static_cast<int>(i);
                    target_session = row;
                }
            }
        }

        // Runaway guard
        if (target_row_idx >= 0 && readable_rows > 1 && keepable_rows == 0) {
            std::cout << "  [ABORT] Every row looked unprotected, which is almost certainly a misread page. Nothing terminated." << std::endl;
            break;
        }

        if (target_row_idx >= 0) {
            std::string session_key = target_session.device_text + "_" + target_session.location_text + "_" + target_session.created_text;
            if (options_.dry_run) {
                std::cout << "  [WOULD TERMINATE] " << target_session.device_text
                          << " | Location: " << (target_session.location_text.empty() ? "Unknown" : target_session.location_text)
                          << " | Created: " << target_session.created_text << std::endl;
                processed_keys.insert(session_key);
                res.terminated++;
                continue_scanning = false;
            } else {
                std::cout << "  [TERMINATING] " << target_session.device_text
                          << " | Location: " << (target_session.location_text.empty() ? "Unknown" : target_session.location_text)
                          << " | Created: " << target_session.created_text << "..." << std::endl;

                // Execute termination in DOM
                std::string term_script =
                    "(async (rowIdx) => {\n"
                    "    const table = document.querySelector('table[data-chokidar-sessions=\"1\"]');\n"
                    "    if (!table) return {success: false, error: 'Table not found'};\n"
                    "    const rows = table.querySelectorAll('tbody tr, tr:not(:first-child)');\n"
                    "    const row = rows[rowIdx];\n"
                    "    if (!row) return {success: false, error: 'Row vanished'};\n"
                    "    const actionBtn = row.querySelector('button[aria-label^=\"Session actions\"], button[aria-haspopup=\"menu\"], button');\n"
                    "    if (!actionBtn) return {success: false, error: 'Action button not found'};\n"
                    "    actionBtn.click();\n"
                    "    await new Promise(r => setTimeout(r, 500));\n"
                    "    const menuItems = [...document.querySelectorAll('[role=\"menuitem\"], [role=\"menu\"] button, [data-part=\"item\"]')];\n"
                    "    const termItem = menuItems.find(el => /terminate/i.test(el.innerText || el.textContent || ''));\n"
                    "    if (!termItem) return {success: false, error: 'Terminate menu item not found'};\n"
                    "    termItem.click();\n"
                    "    await new Promise(r => setTimeout(r, 600));\n"
                    "    const dialog = document.querySelector('[role=\"alertdialog\"], [role=\"dialog\"]');\n"
                    "    if (!dialog) return {success: false, error: 'Confirmation dialog not found'};\n"
                    "    const buttons = [...dialog.querySelectorAll('button')];\n"
                    "    const confirmBtn = buttons.find(b => /^\\s*terminate\\s*$/i.test((b.innerText || b.textContent || '').trim()));\n"
                    "    if (!confirmBtn) return {success: false, error: 'Confirm button not found'};\n"
                    "    confirmBtn.click();\n"
                    "    return {success: true};\n"
                    "})(" + std::to_string(target_row_idx) + ")";

                JsonValue term_res = cdp_->evaluate(term_script);
                if (term_res["success"].as_bool()) {
                    sleep_ms(1500);
                    std::cout << "  --> Successfully terminated session!" << std::endl;
                    res.terminated++;
                    continue_scanning = true;
                } else {
                    std::cout << "  --> Error terminating session: " << term_res["error"].as_string() << std::endl;
                }
            }
        }
    }

    // Process remainder if in dry-run
    if (options_.dry_run) {
        JsonValue final_rows = cdp_->evaluate(read_rows_script);
        if (final_rows.is_array()) {
            for (size_t i = 0; i < final_rows.size(); ++i) {
                const auto& r = final_rows[i];
                SessionRow row;
                row.device_text = r["deviceText"].as_string();
                row.location_text = r["locationText"].as_string();
                row.created_text = r["createdText"].as_string();
                row.is_current = r["isCurrent"].as_bool();
                std::string session_key = row.device_text + "_" + row.location_text + "_" + row.created_text;
                if (processed_keys.find(session_key) != processed_keys.end()) continue;

                auto verdict = classify_session(row, options_.allowed_locations);
                if (verdict.keep) {
                    std::cout << "  [KEEP]  " << replace_all(row.device_text, "\n", " ")
                              << " | Location: " << (row.location_text.empty() ? "N/A" : row.location_text)
                              << " | " << verdict.reason << std::endl;
                    res.kept++;
                } else {
                    std::cout << "  [WOULD TERMINATE] " << row.device_text
                              << " | Location: " << (row.location_text.empty() ? "Unknown" : row.location_text)
                              << " | Created: " << row.created_text << std::endl;
                    res.terminated++;
                }
                processed_keys.insert(session_key);
            }
        }
    }

    res.scanned = static_cast<int>(processed_keys.size());
    std::cout << "[" << time << "] Check complete. Total: " << res.scanned
              << " | Kept: " << res.kept
              << " | " << (options_.dry_run ? "Flagged" : "Terminated") << ": " << res.terminated << std::endl;

    return res;
}

int Bot::run() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << std::string(65, '=') << "\n"
              << "  Claude Active Sessions Manager - Bot Mode (C++ Edition)\n"
              << std::string(65, '=') << "\n"
              << "Protected (always): outside India, and in India: ";
    for (size_t i = 0; i < PROTECTED_LOCATIONS.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << PROTECTED_LOCATIONS[i];
    }
    std::cout << "\n";

    std::vector<std::string> extras;
    for (const auto& a : options_.allowed_locations) {
        bool is_builtin = false;
        for (const auto& p : PROTECTED_LOCATIONS) {
            if (p == a) { is_builtin = true; break; }
        }
        if (!is_builtin) extras.push_back(a);
    }
    if (!extras.empty()) {
        std::cout << "Protected (extra) : ";
        for (size_t i = 0; i < extras.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << extras[i];
        }
        std::cout << "\n";
    }

    std::cout << "Mode              : " << (options_.dry_run ? "DRY-RUN (Preview only)" : "LIVE (Auto-terminate unprotected)") << "\n"
              << "Interval          : " << (options_.once ? "Single scan (--once)" : "Every " + std::to_string(options_.interval) + "s") << "\n"
              << "Session Dir       : " << options_.user_data_dir << "\n"
              << std::string(65, '=') << std::endl;

    // Start health server if PORT env variable is set
    const char* port_env = std::getenv("PORT");
    if (port_env && *port_env) {
        int port = std::atoi(port_env);
        if (port > 0) {
            health_server_ = std::make_unique<HealthServer>(port, &stats_);
            health_server_->start();
        }
    }

    if (!open_browser()) {
        cleanup();
        return 1;
    }

    if (!check_authentication()) {
        std::cerr << "\nNot signed in to Claude (timed out). Please sign in or provide valid SESSION_KEY.\n";
        cleanup();
        return 1;
    }

    std::cout << "Authentication confirmed! Bot is active." << std::endl;

    int check_count = 1;
    while (!is_shutting_down_) {
        ScanResult res = perform_scan(check_count++);
        stats_.scans++;
        stats_.kept += res.kept;
        stats_.terminated += res.terminated;

        if (options_.once) {
            std::cout << "\nSingle scan completed (--once). Exiting." << std::endl;
            break;
        }

        std::cout << "\n⏳ Next check in " << options_.interval << "s (Press Ctrl+C to stop)..." << std::endl;

        // Sleep in 500ms increments to handle signals quickly
        int slept = 0;
        while (slept < options_.interval * 1000 && !is_shutting_down_) {
            sleep_ms(500);
            slept += 500;
        }
    }

    std::cout << "\n\nStopping bot and cleaning up browser session...\n";
    int mins = static_cast<int>((now_ms() - stats_.started_at) / 60000);
    std::string verb = options_.dry_run ? "Flagged" : "Terminated";
    std::cout << "Session stats: " << stats_.scans << " scans | " << verb << ": "
              << stats_.terminated << " | Kept: " << stats_.kept << " | Uptime: " << mins << "m\n"
              << "Bot shutdown cleanly. Bye!" << std::endl;

    cleanup();
    return 0;
}

} // namespace chokidar
