#include "console.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "debug_server.h"
#include "log.h"

extern char** environ;

namespace bg3le {
namespace {

std::atomic<bool> g_opened{false};

std::string dirname_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool executable(const std::string& path) {
    return ::access(path.c_str(), X_OK) == 0;
}

// Resolve a bare command against PATH.
bool on_path(const std::string& cmd) {
    if (cmd.find('/') != std::string::npos) return executable(cmd);
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string remaining(path);
    while (!remaining.empty()) {
        const std::size_t colon = remaining.find(':');
        const std::string dir = remaining.substr(0, colon);
        if (!dir.empty() && executable(dir + "/" + cmd)) return true;
        if (colon == std::string::npos) break;
        remaining.erase(0, colon + 1);
    }
    return false;
}

// Our own .so lives in <root>/build/, and the client submodule in
// <root>/client/.
std::string client_path() {
    const char* override_path = std::getenv("BG3LE_CLIENT");
    if (override_path != nullptr) return override_path;

    Dl_info info{};
    if (::dladdr(reinterpret_cast<void*>(&client_path), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    const std::string root = dirname_of(dirname_of(info.dli_fname));
    return root + "/client/bg3lua";
}

bool console_enabled(const std::string& exe_dir) {
    if (const char* env = std::getenv("BG3LE_CONSOLE")) return env[0] == '1';
    return settings_flag(exe_dir, "CreateConsole", false);
}

// Inside the Steam runtime container the host's terminals are neither on
// PATH nor loadable (their libraries are not visible), but
// steam-runtime-launch-client can run a command back on the host.
std::string host_path_of(const std::string& cmd) {
    if (!on_path("steam-runtime-launch-client")) return {};
    for (const char* dir : {"/usr/bin/", "/usr/local/bin/", "/bin/"}) {
        const std::string host = std::string("/run/host") + dir + cmd;
        if (executable(host)) return std::string(dir) + cmd;
    }
    return {};
}

struct Launch {
    std::string command;
    bool via_host = false;
};

// $TERMINAL, then a guess from the desktop, then generic fallbacks -- each tried inside
// the container first, then on the host.
Launch pick_terminal() {
    std::vector<std::string> candidates;

    if (const char* env = std::getenv("TERMINAL")) {
        if (env[0] != '\0') candidates.emplace_back(env);
    }

    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) {
        const std::string d(desktop);
        const struct { const char* match; const char* term; } table[] = {
            {"KDE", "konsole"},         {"GNOME", "gnome-terminal"},
            {"XFCE", "xfce4-terminal"}, {"MATE", "mate-terminal"},
            {"LXQt", "qterminal"},      {"Cinnamon", "gnome-terminal"},
            {"Deepin", "deepin-terminal"}, {"Hyprland", "foot"},
            {"sway", "foot"},
        };
        for (const auto& entry : table) {
            if (d.find(entry.match) != std::string::npos) {
                candidates.emplace_back(entry.term);
            }
        }
    }

    candidates.emplace_back("alacritty");
    candidates.emplace_back("xterm");

    for (const std::string& candidate : candidates) {
        if (on_path(candidate)) return {candidate, false};
        const std::string host = host_path_of(candidate);
        if (!host.empty()) return {host, true};
    }
    return {"xterm", false};
}

// Terminals disagree on how a command is passed.
std::vector<std::string> build_argv(const std::string& term,
                                    const std::string& client) {
    const std::size_t slash = term.find_last_of('/');
    const std::string name = slash == std::string::npos ? term : term.substr(slash + 1);

    if (name == "gnome-terminal" || name == "tilix" || name == "ptyxis") {
        return {term, "--", client};
    }
    if (name == "xfce4-terminal" || name == "mate-terminal" || name == "terminator") {
        return {term, "-e", client};  // these take a single command string
    }
    if (name == "kitty" || name == "foot") {
        return {term, client};
    }
    if (name == "wezterm") {
        return {term, "start", "--", client};
    }
    return {term, "-e", client};  // xterm, konsole, alacritty, urxvt, st, ...
}

}  // namespace

// Parity with the Windows extender's settings file.
bool settings_flag(const std::string& exe_dir, const char* key, bool fallback) {
    const std::string settings = exe_dir + "/ScriptExtenderSettings.json";
    std::FILE* f = std::fopen(settings.c_str(), "rb");
    if (f == nullptr) return fallback;
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const std::string quoted = std::string("\"") + key + "\"";
    const std::size_t at = text.find(quoted);
    if (at == std::string::npos) return fallback;
    const std::size_t colon = text.find(':', at);
    if (colon == std::string::npos) return fallback;
    const std::size_t value = text.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos) return fallback;
    if (text.compare(value, 4, "true") == 0) return true;
    if (text.compare(value, 5, "false") == 0) return false;
    return fallback;
}

// A flag from ScriptExtenderSettings.json next to the game binary.
extern "C" bool bg3le_settings_flag(char const* key, bool fallback) {
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return fallback;
    exe[n] = '\0';
    std::string dir(exe);
    const std::size_t slash = dir.rfind('/');
    if (slash != std::string::npos) dir.erase(slash);
    return settings_flag(dir, key, fallback);
}

void maybe_open_console() {
    bool expected = false;
    if (!g_opened.compare_exchange_strong(expected, true)) return;

    char exe[4096];
    const ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return;
    exe[len] = '\0';

    if (!console_enabled(dirname_of(exe))) {
        g_opened.store(false);
        return;
    }

    const std::string client = client_path();
    if (client.empty() || !executable(client)) {
        statusf("CreateConsole: client not found at '%s'", client.c_str());
        return;
    }

    const Launch launch = pick_terminal();
    if (!launch.via_host && !on_path(launch.command)) {
        statusf("CreateConsole: no usable terminal (tried '%s')",
                launch.command.c_str());
        return;
    }

    std::vector<std::string> args = build_argv(launch.command, client);
    if (launch.via_host) {
        // launch-client waits for the host command, and pressure-vessel in
        // turn waits for launch-client -- so without detaching, the game's
        // container cannot exit until the console window is closed.
        std::vector<std::string> prefix{"steam-runtime-launch-client", "--host", "--"};
        if (executable("/run/host/usr/bin/setsid")) {
            prefix.emplace_back("/usr/bin/setsid");
            prefix.emplace_back("-f");
        } else {
            statusf("CreateConsole: setsid missing on host; the game will not "
                    "exit until the console is closed");
        }
        args.insert(args.begin(), prefix.begin(), prefix.end());
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // What the launcher and the terminal say, so a window that never
    // appears leaves a reason behind.
    char outPath[128];
    std::snprintf(outPath, sizeof(outPath), "/tmp/bg3le-console.%d.log",
                  (int)::getpid());
    posix_spawn_file_actions_t files;
    ::posix_spawn_file_actions_init(&files);
    ::posix_spawn_file_actions_addopen(&files, 1, outPath,
                                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ::posix_spawn_file_actions_adddup2(&files, 1, 2);

    pid_t pid = 0;
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    // Without LD_PRELOAD: the launcher and the terminal have no use for bg3le.
    std::vector<char*> env;
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
    }
    env.push_back(nullptr);
    const int rc = ::posix_spawnp(&pid, argv[0], &files, &attr, argv.data(), env.data());
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&files);

    if (rc != 0) {
        statusf("CreateConsole: failed to launch %s (%s)", launch.command.c_str(),
                std::strerror(rc));
        return;
    }

    // Reap it ourselves rather than touching the game's SIGCHLD handling.
    std::thread([pid] {
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            logf("CreateConsole: launcher killed by signal %d", WTERMSIG(status));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            logf("CreateConsole: launcher exited with status %d",
                 WEXITSTATUS(status));
        }
    }).detach();
    statusf("CreateConsole: opened %s%s running %s", launch.command.c_str(),
            launch.via_host ? " (on host)" : "", client.c_str());
}

}  // namespace bg3le
