#include "bot.hpp"
#include "config.hpp"
#include "chrome_finder.hpp"
#include "common.hpp"
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/types.h>

using namespace chokidar;

int main(int argc, char* argv[]) {
    Options options = parse_args(argc, argv);

    if (options.help) {
        show_help(argv[0]);
        return 0;
    }

    if (options.list_profiles) {
        std::string user_data = default_user_data_dir();
        auto profiles = list_chrome_profiles(user_data);
        std::cout << "Chrome user data: " << user_data << "\n";
        if (profiles.empty()) {
            std::cout << "No Chrome profiles found.\n";
            return 0;
        }
        for (const auto& p : profiles) {
            std::string marks;
            if (p.is_last_used) marks += "last used";
            if (p.is_locked) marks += (marks.empty() ? "" : ", ") + std::string("open in Chrome");
            std::cout << "  " << std::left
                      << p.dir << "   "
                      << p.name << "   "
                      << p.email << "   "
                      << marks << "\n";
        }
        return 0;
    }

    if (!options.save_session_key_arg.empty()) {
        std::string raw = options.save_session_key_arg;
        if (raw == "interactive") {
            std::cout << "Paste your claude.ai session cookie(s): ";
            std::string line;
            if (std::getline(std::cin, line)) {
                raw = trim(line);
            }
        }
        return save_session_key_file(raw) ? 0 : 1;
    }

    if (options.launch_chrome_debug) {
        int port = 9222;
        if (is_port_open(port)) {
            std::cout << "Chrome is already listening for debugging on port " << port << ".\n";
            std::cout << "Run:  ./claude-chokidar --cdp " << port << "\n";
            return 0;
        }

        std::string chrome_exe = find_chrome_executable(options.chrome_path);
        if (chrome_exe.empty()) {
            std::cerr << "Could not find chrome. Set CHROME_PATH and retry.\n";
            return 1;
        }

        std::string user_data = default_user_data_dir();
        auto profile = resolve_chrome_profile(user_data, options.chrome_profile);
        if (profile.dir.empty()) {
            std::cerr << "No Chrome profile matched \"" << options.chrome_profile << "\".\n";
            return 1;
        }

        if (is_chrome_running()) {
            std::cerr << "Chrome is currently running. Close every Chrome window first, then re-run this.\n"
                      << "(Debugging cannot be enabled on a Chrome that is already up.)\n";
            return 1;
        }

        std::cout << "Chrome     : " << chrome_exe << "\n"
                  << "Profile    : " << profile.dir << " (" << (profile.email.empty() ? profile.name : profile.email) << ")\n"
                  << "Debug port : " << port << "\n";

        pid_t pid = fork();
        if (pid == 0) {
            std::vector<std::string> args = {
                chrome_exe,
                "--remote-debugging-port=" + std::to_string(port),
                "--user-data-dir=" + user_data,
                "--profile-directory=" + profile.dir,
                "--no-first-run",
                "--no-default-browser-check",
                SETTINGS_URL
            };
            std::vector<char*> c_args;
            for (auto& a : args) c_args.push_back(const_cast<char*>(a.c_str()));
            c_args.push_back(nullptr);

            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
            execvp(c_args[0], c_args.data());
            _exit(1);
        }

        int64_t deadline = now_ms() + 20000;
        while (now_ms() < deadline) {
            if (is_port_open(port)) {
                std::cout << "\nChrome is up with debugging on " << port << ". Now run:\n"
                          << "  ./claude-chokidar --cdp " << port << "\n";
                return 0;
            }
            sleep_ms(500);
        }

        std::cerr << "\nChrome started but never opened the debug port.\n"
                  << "Chrome 136+ blocks remote debugging when running on the default profile directory.\n"
                  << "Use the bot profile instead (sign in once, it is then remembered):\n"
                  << "  ./claude-chokidar\n";
        return 1;
    }

    Bot bot(options);
    return bot.run();
}
