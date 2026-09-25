// Ext.IO.AddPathOverride, honoured where upstream honours it: in
// ls::FileReader's constructor (ScriptExtender::OnFileReaderCreate), which
// every engine file open goes through. Keys are absolute, as upstream makes
// them with ToPath(path, Data). Adapted from bg3se (by Norbyte and the bg3se
// contributors) -- thank you.

#include <stdafx.h>

#include <GameDefinitions/Symbols.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "../hook.h"
#include "../log.h"
#include "../mem.h"

namespace {

// ls::FileReader::FileReader(Path const&, type, unknown): zeroes the reader,
// resolves the path to a file id (image+0x2704210) and loads it; 75 callers.
constexpr std::uintptr_t kFileReaderCtor = 0x2704030;
constexpr unsigned char kFileReaderCtorHead[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10,
    0x89, 0xd5, 0x0f, 0x57, 0xc0, 0x48, 0xb8, 0x01, 0x01, 0x08, 0x00, 0x01};

using CtorProc = void* (*)(void* self, bg3se::Path const* path, unsigned type, unsigned unknown);
CtorProc g_real = nullptr;

std::shared_mutex g_lock;
std::unordered_map<std::string, std::string> g_overrides;
std::atomic<bool> g_any{false};

std::atomic<int> g_trace{-1};

void* file_reader_ctor(void* self, bg3se::Path const* path, unsigned type, unsigned unknown) {
    // BG3LE_TRACE_FILES=N logs the first N paths opened.
    int left = g_trace.load(std::memory_order_relaxed);
    if (left < 0) {
        char const* env = std::getenv("BG3LE_TRACE_FILES");
        left = env != nullptr ? std::atoi(env) : 0;
        g_trace.store(left);
    }
    if (left > 0 && path != nullptr && g_trace.fetch_sub(1) > 0) {
        bg3le::logf("io: FileReader %.*s (type %u)", (int)path->Name.size(), path->Name.data(), type);
    }
    if (g_any.load(std::memory_order_relaxed) && path != nullptr) {
        std::shared_lock<std::shared_mutex> held(g_lock);
        auto it = g_overrides.find(std::string(path->Name.data(), path->Name.size()));
        if (it != g_overrides.end()) {
            bg3se::Path overridden;
            overridden.Name = it->second.c_str();
            held.unlock();
            return g_real(self, &overridden, type, unknown);
        }
    }
    return g_real(self, path, type, unknown);
}

std::string absolute(char const* path) {
    if (bg3se::gStaticSymbols == nullptr) return path;
    auto p = bg3se::gStaticSymbols->ToPath(path, bg3se::PathRootType::Data);
    return std::string(p.data(), p.size());
}

// ls::FileReader::~FileReader, which closes and releases what it loaded.
constexpr std::uintptr_t kFileReaderDtor = 0x27036f0;
constexpr unsigned char kFileReaderDtorHead[] = {
    0x41, 0x56, 0x53, 0x50, 0x0f, 0x57, 0xc0, 0x48, 0x89, 0xfb,
    0x0f, 0x11, 0x47, 0x08, 0x4c, 0x8b, 0xb7, 0x88, 0x00, 0x00};
constexpr std::size_t kFileReaderSize = 0x90;  // the engine's callers' stack slot
void (*g_dtor)(void*) = nullptr;

}  // namespace

namespace bg3le {

// Upstream's script::LoadExternalFile for PathRootType::Data: a FileReader
// on ToPath(path, Data), through the hook so overrides apply.
bool engine_read_file(char const* relative, std::string* out) {
    if (g_real == nullptr || g_dtor == nullptr || relative == nullptr) return false;
    alignas(16) unsigned char reader[kFileReaderSize] = {};
    bg3se::Path path;
    path.Name = absolute(relative).c_str();
    file_reader_ctor(reader, &path, 1, 0);
    const bool loaded = reader[0] != 0;
    if (loaded) {
        char const* data = nullptr;
        std::uint64_t size = 0;
        std::memcpy(&data, reader + 0x8, sizeof(data));
        std::memcpy(&size, reader + 0x18, sizeof(size));
        if (data != nullptr) out->assign(data, size);
    }
    g_dtor(reader);
    return loaded;
}

void install_path_override_hook() {
    unsigned char held[sizeof(kFileReaderCtorHead)] = {};
    if (!safe_read((void const*)(load_bias() + kFileReaderCtor), held, sizeof(held))
        || std::memcmp(held, kFileReaderCtorHead, sizeof(held)) != 0) {
        logf("io: ls::FileReader's constructor is not at image+%#lx; path overrides stay unhonoured",
             (unsigned long)kFileReaderCtor);
        return;
    }
    void* original = nullptr;
    if (hook_call_sites(kFileReaderCtor, reinterpret_cast<void*>(&file_reader_ctor), &original, true) > 0) {
        g_real = reinterpret_cast<CtorProc>(original);
    }
    unsigned char dtor[sizeof(kFileReaderDtorHead)] = {};
    if (safe_read((void const*)(load_bias() + kFileReaderDtor), dtor, sizeof(dtor))
        && std::memcmp(dtor, kFileReaderDtorHead, sizeof(dtor)) == 0) {
        g_dtor = reinterpret_cast<void (*)(void*)>(load_bias() + kFileReaderDtor);
    }
}

}  // namespace bg3le

// Upstream's ScriptExtender::AddPathOverride. False if the hook is not in.
extern "C" bool bg3le_path_override_add(char const* path, char const* overridePath) {
    if (g_real == nullptr || path == nullptr || overridePath == nullptr) return false;
    std::string from = absolute(path), to = absolute(overridePath);
    std::unique_lock<std::shared_mutex> held(g_lock);
    g_overrides[std::move(from)] = std::move(to);
    g_any.store(true, std::memory_order_relaxed);
    return true;
}

// Upstream's GetPathOverride: the absolute override, or null.
extern "C" char const* bg3le_path_override_get(char const* path) {
    static thread_local std::string out;
    if (path == nullptr) return nullptr;
    const std::string key = absolute(path);
    std::shared_lock<std::shared_mutex> held(g_lock);
    auto it = g_overrides.find(key);
    if (it == g_overrides.end()) return nullptr;
    out = it->second;
    return out.c_str();
}

// Upstream's ClearPathOverrides, on an extension state reset.
extern "C" void bg3le_path_override_clear() {
    std::unique_lock<std::shared_mutex> held(g_lock);
    g_overrides.clear();
    g_any.store(false, std::memory_order_relaxed);
}
