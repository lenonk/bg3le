// Native plugins: shared libraries in ~/.local/share/bg3le/plugins, loaded
// when the game's event loop starts. See include/bg3le_plugin.h for the API
// and README.md for how plugins are installed.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../include/bg3le_plugin.h"
#include "debug_server.h"
#include "lauxlib.h"
#include "log.h"
#include "lua.h"

#ifndef BG3LE_VERSION
#define BG3LE_VERSION "unknown"
#endif

struct bg3le_plugin {
    std::string file;      // full path of the .so
    std::string name;      // describe()'s, or the file name
    std::string version;
    std::string error;     // why it isn't running, or ""
    std::string settings_path;
    bool loaded = false;
    struct Setting {
        std::string id;
        bg3le_setting_type type;
        void* value;
        double min, max;
    };
    std::vector<Setting> settings;
    std::map<std::string, double> saved;  // the settings file, read before init
};

namespace bg3le {

namespace {

std::recursive_mutex g_mutex;
std::vector<std::unique_ptr<bg3le_plugin>> g_plugins;
struct Handler {
    bg3le_plugin* owner;
    bg3le_event_handler fn;
    void* user;
};
// Added during loading and only read afterwards, both on the main thread.
std::vector<Handler> g_handlers;
struct FrameHandler {
    bg3le_plugin* owner;
    bg3le_frame_handler fn;
    void* user;
};
// Added during loading on the main thread; read on the client's game thread
// once g_frames_ready publishes them.
std::vector<FrameHandler> g_frame_handlers;
std::atomic<bool> g_frames_ready{false};

std::string plugins_dir() {
    if (const char* dir = std::getenv("BG3LE_PLUGINS_DIR")) return dir;
    std::string data;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        data = xdg;
    } else if (const char* home = std::getenv("HOME")) {
        data = std::string(home) + "/.local/share";
    } else {
        return "";
    }
    return data + "/bg3le/plugins";
}

std::string base_name(std::string const& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The settings file: one flat JSON object of numbers and booleans, which is
// all bg3le writes.
std::map<std::string, double> read_settings(std::string const& path) {
    std::map<std::string, double> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    std::string text;
    char block[4096];
    for (size_t got; (got = std::fread(block, 1, sizeof(block), f)) > 0;) text.append(block, got);
    std::fclose(f);

    size_t at = 0;
    while ((at = text.find('"', at)) != std::string::npos) {
        size_t end = text.find('"', at + 1);
        if (end == std::string::npos) break;
        std::string key = text.substr(at + 1, end - at - 1);
        size_t colon = text.find(':', end);
        if (colon == std::string::npos) break;
        const char* v = text.c_str() + colon + 1;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        char* stop = nullptr;
        if (std::strncmp(v, "true", 4) == 0) {
            out[key] = 1;
        } else if (std::strncmp(v, "false", 5) == 0) {
            out[key] = 0;
        } else {
            double d = std::strtod(v, &stop);
            if (stop != v) out[key] = d;
        }
        at = colon + 1;
    }
    return out;
}

double read_value(bg3le_plugin::Setting const& s) {
    switch (s.type) {
        case BG3LE_SETTING_FLOAT: return *static_cast<float*>(s.value);
        default: return *static_cast<int*>(s.value);
    }
}

// Clamped to the setting's range, and to 0/1 for a bool.
void write_value(bg3le_plugin::Setting const& s, double v) {
    if (s.min < s.max) v = std::clamp(v, s.min, s.max);
    switch (s.type) {
        case BG3LE_SETTING_BOOL: *static_cast<int*>(s.value) = v != 0 ? 1 : 0; break;
        case BG3LE_SETTING_INT: *static_cast<int*>(s.value) = (int)std::lround(v); break;
        case BG3LE_SETTING_FLOAT: *static_cast<float*>(s.value) = (float)v; break;
    }
}

void save_settings(bg3le_plugin const& p) {
    if (p.settings_path.empty()) return;
    std::string text = "{\n";
    for (size_t i = 0; i < p.settings.size(); i++) {
        auto const& s = p.settings[i];
        char value[64];
        if (s.type == BG3LE_SETTING_BOOL) {
            std::snprintf(value, sizeof(value), "%s", read_value(s) != 0 ? "true" : "false");
        } else if (s.type == BG3LE_SETTING_INT) {
            std::snprintf(value, sizeof(value), "%d", (int)read_value(s));
        } else {
            std::snprintf(value, sizeof(value), "%.9g", read_value(s));
        }
        text += "  \"" + s.id + "\": " + value + (i + 1 < p.settings.size() ? ",\n" : "\n");
    }
    text += "}\n";
    std::string tmp = p.settings_path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) std::rename(tmp.c_str(), p.settings_path.c_str());
    else std::remove(tmp.c_str());
}

// ---- the host table ----

void host_describe(bg3le_plugin* self, const char* name, const char* version) {
    std::lock_guard<std::recursive_mutex> held(g_mutex);
    if (name && *name) self->name = name;
    if (version) self->version = version;
}

void host_vlog(bg3le_plugin* self, bool warn, const char* fmt, va_list args) {
    char line[1024];
    std::vsnprintf(line, sizeof(line), fmt, args);
    if (warn) statusf("WARNING: plugin %s: %s", self->name.c_str(), line);
    else logf("plugin %s: %s", self->name.c_str(), line);
}

void host_log(bg3le_plugin* self, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    host_vlog(self, false, fmt, args);
    va_end(args);
}

void host_warn(bg3le_plugin* self, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    host_vlog(self, true, fmt, args);
    va_end(args);
}

int host_add_event_handler(bg3le_plugin* self, bg3le_event_handler fn, void* user) {
    if (fn == nullptr) return -1;
    g_handlers.push_back({self, fn, user});
    return 0;
}

int host_add_frame_handler(bg3le_plugin* self, bg3le_frame_handler fn, void* user) {
    // The client thread reads the list without a lock once loading is done.
    if (fn == nullptr || g_frames_ready.load(std::memory_order_acquire)) return -1;
    g_frame_handlers.push_back({self, fn, user});
    return 0;
}

// From bg3le's own object, so RTLD_NEXT is SDL's real function, past ours.
void* host_sdl_function(const char* name) {
    if (void* next = ::dlsym(RTLD_NEXT, name)) return next;
    if (void* h = ::dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD)) return ::dlsym(h, name);
    return nullptr;
}

int host_add_setting(bg3le_plugin* self, const char* id, bg3le_setting_type type, void* value,
                     double min, double max) {
    if (id == nullptr || *id == '\0' || value == nullptr || type < BG3LE_SETTING_BOOL
        || type > BG3LE_SETTING_FLOAT) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> held(g_mutex);
    for (auto const& s : self->settings) {
        if (s.id == id) return -1;
    }
    self->settings.push_back({id, type, value, min, max});
    auto saved = self->saved.find(id);
    if (saved != self->saved.end()) write_value(self->settings.back(), saved->second);
    return 0;
}

const bg3le_host g_host = {
    BG3LE_PLUGIN_ABI,
    sizeof(bg3le_host),
    BG3LE_VERSION,
    host_describe,
    host_log,
    host_warn,
    host_add_event_handler,
    host_sdl_function,
    host_add_setting,
    host_add_frame_handler,
};

bg3le_plugin* find_plugin(const char* name) {
    for (auto& p : g_plugins) {
        if (p->name == name) return p.get();
    }
    return nullptr;
}

bg3le_plugin::Setting* find_setting(bg3le_plugin* p, const char* id) {
    for (auto& s : p->settings) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

}  // namespace

// On the game's first SDL_PollEvent: the engine and its allocator are up.
void plugins_load() {
    static bool done = false;
    if (done) return;
    done = true;
    // However loading ends, frame handlers are then published and closed.
    struct Publish {
        ~Publish() { g_frames_ready.store(true, std::memory_order_release); }
    } publish;

    const std::string dir = plugins_dir();
    DIR* d = dir.empty() ? nullptr : ::opendir(dir.c_str());
    if (d == nullptr) return;
    std::vector<std::string> files;
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) files.push_back(dir + "/" + name);
    }
    ::closedir(d);
    std::sort(files.begin(), files.end());

    for (std::string const& file : files) {
        auto plugin = std::make_unique<bg3le_plugin>();
        bg3le_plugin* p = plugin.get();
        p->file = file;
        p->name = base_name(file);
        p->settings_path = file.substr(0, file.size() - 3) + ".settings.json";
        p->saved = read_settings(p->settings_path);
        {
            std::lock_guard<std::recursive_mutex> held(g_mutex);
            g_plugins.push_back(std::move(plugin));
        }

        void* handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            p->error = ::dlerror();
            statusf("WARNING: plugin %s did not load: %s", p->name.c_str(), p->error.c_str());
            continue;
        }
        using Init = int (*)(const bg3le_host*, bg3le_plugin*);
        auto init = reinterpret_cast<Init>(::dlsym(handle, "bg3le_plugin_init"));
        if (init == nullptr) {
            p->error = "no bg3le_plugin_init";
            statusf("WARNING: plugin %s has no bg3le_plugin_init; ignored", p->name.c_str());
            continue;
        }
        int rc = init(&g_host, p);
        if (rc != 0) {
            // Left loaded: it may have patched code that jumps into it.
            std::lock_guard<std::recursive_mutex> held(g_mutex);
            p->error = "bg3le_plugin_init returned " + std::to_string(rc);
            p->settings.clear();
            g_handlers.erase(std::remove_if(g_handlers.begin(), g_handlers.end(),
                                            [p](Handler const& h) { return h.owner == p; }),
                             g_handlers.end());
            g_frame_handlers.erase(std::remove_if(g_frame_handlers.begin(), g_frame_handlers.end(),
                                                  [p](FrameHandler const& h) { return h.owner == p; }),
                                   g_frame_handlers.end());
            statusf("WARNING: plugin %s failed to start (%s)", p->name.c_str(), p->error.c_str());
            continue;
        }
        p->loaded = true;
        for (auto const& [id, value] : p->saved) {
            if (find_setting(p, id.c_str()) == nullptr) {
                statusf("WARNING: plugin %s has no setting %s (in %s)", p->name.c_str(), id.c_str(),
                        p->settings_path.c_str());
            }
        }
        // Written every start, so the file always lists every setting for hand editing.
        if (!p->settings.empty()) save_settings(*p);
        logf("plugin %s %s loaded from %s (%zu settings)", p->name.c_str(), p->version.c_str(),
             file.c_str(), p->settings.size());
    }
}

// Whether a plugin kept the event from the game.
bool plugins_dispatch_event(SDL_Event* event) {
    for (Handler const& h : g_handlers) {
        if (h.fn(h.user, event) != 0) return true;
    }
    return false;
}

// Once per client frame, on the client's game thread.
void plugins_dispatch_frame() {
    if (!g_frames_ready.load(std::memory_order_acquire) || g_frame_handlers.empty()) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    for (FrameHandler const& h : g_frame_handlers) h.fn(h.user, dt);
}

}  // namespace bg3le

using namespace bg3le;

// Ext._Internal.PluginList() -> { {Name, Version, File, Loaded, Error}, ... }
extern "C" int bg3le_ext_plugin_list(lua_State* L) {
    std::lock_guard<std::recursive_mutex> held(g_mutex);
    lua_createtable(L, (int)g_plugins.size(), 0);
    int i = 1;
    for (auto const& p : g_plugins) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, p->name.c_str());
        lua_setfield(L, -2, "Name");
        lua_pushstring(L, p->version.c_str());
        lua_setfield(L, -2, "Version");
        lua_pushstring(L, p->file.c_str());
        lua_setfield(L, -2, "File");
        lua_pushboolean(L, p->loaded);
        lua_setfield(L, -2, "Loaded");
        lua_pushstring(L, p->error.c_str());
        lua_setfield(L, -2, "Error");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

namespace {

void push_value(lua_State* L, bg3le_plugin::Setting const& s) {
    double v = read_value(s);
    if (s.type == BG3LE_SETTING_BOOL) lua_pushboolean(L, v != 0);
    else if (s.type == BG3LE_SETTING_INT) lua_pushinteger(L, (lua_Integer)v);
    else lua_pushnumber(L, v);
}

}  // namespace

// Ext._Internal.PluginSettings(name) -> { {Id, Type, Value, Min, Max}, ... } or nil
extern "C" int bg3le_ext_plugin_settings(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::lock_guard<std::recursive_mutex> held(g_mutex);
    bg3le_plugin* p = find_plugin(name);
    if (p == nullptr) return 0;
    lua_createtable(L, (int)p->settings.size(), 0);
    int i = 1;
    for (auto const& s : p->settings) {
        static const char* const kTypes[] = {"bool", "int", "float"};
        lua_createtable(L, 0, 5);
        lua_pushstring(L, s.id.c_str());
        lua_setfield(L, -2, "Id");
        lua_pushstring(L, kTypes[s.type]);
        lua_setfield(L, -2, "Type");
        push_value(L, s);
        lua_setfield(L, -2, "Value");
        lua_pushnumber(L, s.min);
        lua_setfield(L, -2, "Min");
        lua_pushnumber(L, s.max);
        lua_setfield(L, -2, "Max");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// Ext._Internal.PluginGet(name, id) -> value or nil
extern "C" int bg3le_ext_plugin_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* id = luaL_checkstring(L, 2);
    std::lock_guard<std::recursive_mutex> held(g_mutex);
    bg3le_plugin* p = find_plugin(name);
    bg3le_plugin::Setting* s = p ? find_setting(p, id) : nullptr;
    if (s == nullptr) return 0;
    push_value(L, *s);
    return 1;
}

// Ext._Internal.PluginSet(name, id, value) -> true, or false and why. Saves.
extern "C" int bg3le_ext_plugin_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* id = luaL_checkstring(L, 2);
    double v;
    if (lua_isboolean(L, 3)) v = lua_toboolean(L, 3) ? 1 : 0;
    else if (lua_isnumber(L, 3)) v = lua_tonumber(L, 3);
    else return luaL_argerror(L, 3, "expected a number or a boolean");

    std::lock_guard<std::recursive_mutex> held(g_mutex);
    bg3le_plugin* p = find_plugin(name);
    if (p == nullptr || !p->loaded) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "no running plugin named %s", name);
        return 2;
    }
    bg3le_plugin::Setting* s = find_setting(p, id);
    if (s == nullptr) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "plugin %s has no setting %s", name, id);
        return 2;
    }
    write_value(*s, v);
    save_settings(*p);
    lua_pushboolean(L, 1);
    return 1;
}
