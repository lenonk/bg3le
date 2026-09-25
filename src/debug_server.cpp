#include "debug_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "console.h"
#include "log.h"
#include "lua_host.h"
#include "pb.h"

namespace bg3le {

bool story_ready();  // src/preload.cpp

namespace {

// LuaDebug.proto field numbers.
constexpr unsigned kMsgConnect = 3;
constexpr unsigned kMsgEvaluate = 7;
constexpr unsigned kBkConnectResponse = 3;
constexpr unsigned kBkEvaluateResponse = 5;
constexpr unsigned kBkDebugOutput = 8;

constexpr unsigned kProtocolVersion = 4;
constexpr unsigned kValueTypeString = 4;

std::atomic<bool> g_running{false};

// Several clients at once: CreateConsole opens one automatically, which
// would otherwise occupy the only slot and lock out an ad-hoc session.
// The protocol is single-client per connection, not per server.
std::mutex g_clients_mutex;
std::vector<int> g_clients;
std::mutex g_send_mutex;

struct Job {
    std::string code;
    std::string result;
    std::string error;
    bool client = false;  // which Lua context to evaluate in
    bool done = false;
};

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<std::shared_ptr<Job>> g_queue;

// Pending count is read on every clock_gettime, so keep it lock-free.
std::atomic<int> g_pending{0};
std::atomic<long> g_story_tid{0};

// Clients attach long after load, so retain recent status for replay.
constexpr std::size_t kStatusHistory = 128;
std::mutex g_status_mutex;
std::deque<std::string> g_status;

long this_tid() {
    static thread_local long tid = ::syscall(SYS_gettid);
    return tid;
}

// Framing is a native little-endian uint32 holding the TOTAL packet size,
// the 4-byte length field included.
bool send_packet(int fd, const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_send_mutex);
    const std::uint32_t total = static_cast<std::uint32_t>(payload.size() + 4);
    std::string frame(reinterpret_cast<const char*>(&total), 4);
    frame.append(payload);

    std::size_t sent = 0;
    while (sent < frame.size()) {
        const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

std::string make_connect_response(std::uint64_t reply_seq) {
    std::string body;
    pb::uint_field(&body, 1, kProtocolVersion);
    std::string msg;
    pb::uint_field(&msg, 2, reply_seq);
    pb::bytes_field(&msg, kBkConnectResponse, body);
    return msg;
}

std::string make_evaluate_response(std::uint64_t reply_seq, const std::string& result,
                                   const std::string& error) {
    std::string body;
    if (!error.empty()) {
        pb::bytes_field(&body, 2, error);
    } else {
        std::string value;
        pb::uint_field(&value, 1, kValueTypeString);
        pb::bytes_field(&value, 5, result);
        pb::bytes_field(&body, 1, value);
    }
    std::string msg;
    pb::uint_field(&msg, 2, reply_seq);
    pb::bytes_field(&msg, kBkEvaluateResponse, body);
    return msg;
}

void handle_client(int fd) {
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.push_back(fd);
        logf("debug: client attached (%zu connected)", g_clients.size());
    }

    // Replay what the client missed, so attaching mid-session is informative.
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        for (const std::string& line : g_status) {
            std::string body;
            pb::bytes_field(&body, 1, line);
            std::string msg;
            pb::bytes_field(&msg, kBkDebugOutput, body);
            if (!send_packet(fd, msg)) return;
        }
    }
    for (;;) {
        char header[4];
        if (!recv_exact(fd, header, 4)) break;
        std::uint32_t total = 0;
        std::memcpy(&total, header, 4);
        if (total < 4 || total > (16u << 20)) {
            logf("debug: bad packet size %u", total);
            break;
        }

        std::string payload(total - 4, '\0');
        if (total > 4 && !recv_exact(fd, payload.data(), payload.size())) break;

        std::uint64_t seq = 0;
        std::string connect_body;
        std::string evaluate_body;

        pb::Reader r(payload.data(), payload.size());
        unsigned field = 0;
        unsigned wire = 0;
        while (r.next(&field, &wire)) {
            if (field == 1 && wire == 0) {
                r.read_varint(&seq);
            } else if (field == kMsgConnect && wire == 2) {
                r.read_bytes(&connect_body);
            } else if (field == kMsgEvaluate && wire == 2) {
                r.read_bytes(&evaluate_body);
            } else if (!r.skip(wire)) {
                break;
            }
        }

        if (!connect_body.empty()) {
            if (!send_packet(fd, make_connect_response(seq))) break;
            continue;
        }

        if (evaluate_body.empty()) continue;

        // DbgEvaluate: context=1, expression=2, frame=3, flags=4. The
        // context was parsed and ignored while there was only one Lua
        // state; bg3lua's :client / :server have been sending it all
        // along.
        std::string expression;
        std::uint64_t context = 0;
        pb::Reader er(evaluate_body.data(), evaluate_body.size());
        while (er.next(&field, &wire)) {
            if (field == 1 && wire == 0) {
                er.read_varint(&context);
            } else if (field == 2 && wire == 2) {
                er.read_bytes(&expression);
            } else if (!er.skip(wire)) {
                break;
            }
        }

        // Hand the chunk to the story thread; never touch Lua from here.
        // The server context runs on the story thread, which only exists
        // once a save is loaded; at the main menu nothing would ever run it.
        if (context != 1 && !story_ready()) {
            if (!send_packet(fd, make_evaluate_response(seq, "",
                    "the server context runs once a save is loaded; at the main "
                    "menu, use the client context (:client, or bg3lua --client)"))) {
                break;
            }
            continue;
        }

        auto job = std::make_shared<Job>();
        job->code = expression;
        job->client = context == 1;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_queue.push_back(job);
        }
        g_pending.fetch_add(1, std::memory_order_release);

        std::unique_lock<std::mutex> lock(g_queue_mutex);
        const bool finished = g_queue_cv.wait_for(
            lock, std::chrono::seconds(10), [&job] { return job->done; });
        std::string result = job->result;
        std::string error = job->error;
        lock.unlock();

        if (!finished) {
            error = "timed out waiting for the story thread; is the game paused "
                    "or still loading?";
            // Not left queued: it would run whenever the thread next came
            // round, long after the caller stopped waiting for it.
            std::lock_guard<std::mutex> held(g_queue_mutex);
            auto it = std::find(g_queue.begin(), g_queue.end(), job);
            if (it != g_queue.end()) {
                g_queue.erase(it);
                g_pending.fetch_sub(1, std::memory_order_release);
            }
        }
        if (!send_packet(fd, make_evaluate_response(seq, result, error))) break;
    }

    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), fd),
                        g_clients.end());
        logf("debug: client detached (%zu remaining)", g_clients.size());
    }
    ::close(fd);
}

void listener() {
    const char* env = std::getenv("BG3LE_DEBUG_PORT");
    const int port = env != nullptr ? std::atoi(env) : 9998;

    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        logf("debug: socket() failed");
        return;
    }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(static_cast<std::uint16_t>(port));

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(srv, 1) != 0) {
        logf("debug: cannot listen on 127.0.0.1:%d", port);
        ::close(srv);
        return;
    }
    statusf("Lua debugger listening on 127.0.0.1:%d; DBG protocol version %u",
            port, kProtocolVersion);

    // Only now can a client succeed in connecting.
    maybe_open_console();

    for (;;) {
        const int fd = ::accept(srv, nullptr, nullptr);
        if (fd < 0) break;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::thread(handle_client, fd).detach();
    }
    ::close(srv);
}

}  // namespace

void debug_server_start() {
    const char* opt = std::getenv("BG3LE_DEBUG");
    if (opt != nullptr && opt[0] == '0') return;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    std::thread(listener).detach();
}

namespace {

// Once the client ticks on its own thread, its evaluations run there -- at
// the main menu there is no story thread to run them.
std::atomic<bool> g_client_pumps{false};

void pump_where(bool clientJobs) {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            auto it = g_queue.begin();
            while (it != g_queue.end() && !((*it)->client == clientJobs
                   || (!clientJobs && !g_client_pumps.load()))) {
                ++it;
            }
            if (it == g_queue.end()) return;
            job = *it;
            g_queue.erase(it);
        }

        g_pending.fetch_sub(1, std::memory_order_acq_rel);

        std::string result;
        std::string error;
        lua_eval_in(job->client, job->code.c_str(), &result, &error);

        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            job->result = std::move(result);
            job->error = std::move(error);
            job->done = true;
        }
        g_queue_cv.notify_all();
    }
}

}  // namespace

void debug_server_pump() { pump_where(false); }

void debug_server_pump_client() {
    g_client_pumps.store(true);
    if (g_pending.load(std::memory_order_acquire) == 0) return;
    pump_where(true);
}

void statusf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    logf("%s", buf);
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status.emplace_back(buf);
        if (g_status.size() > kStatusHistory) g_status.pop_front();
    }
    debug_server_output(buf, 0);
}

void debug_server_note_story_thread() {
    long expected = 0;
    g_story_tid.compare_exchange_strong(expected, this_tid());
}

bool debug_server_on_story_thread() {
    const long owner = g_story_tid.load(std::memory_order_relaxed);
    return owner != 0 && this_tid() == owner;
}

void debug_server_tick() {
    if (g_pending.load(std::memory_order_acquire) == 0) return;
    const long owner = g_story_tid.load(std::memory_order_relaxed);
    if (owner == 0 || this_tid() != owner) return;
    debug_server_pump();
}

void debug_server_output(const char* text, int severity) {
    if (text == nullptr) return;
    std::vector<int> targets;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        targets = g_clients;
    }
    if (targets.empty()) return;

    std::string body;
    pb::bytes_field(&body, 1, text);
    pb::uint_field(&body, 2, static_cast<std::uint64_t>(severity));
    std::string msg;
    pb::bytes_field(&msg, kBkDebugOutput, body);
    for (int fd : targets) send_packet(fd, msg);
}

}  // namespace bg3le
