// PersistentVars, Ext.Vars and persistent timers in the savegame, as bg3se's
// SavegameSerializer writes them.
//
// Upstream pre-hooks esv::OsirisVariableHelper::SavegameVisit and visits its
// own "ScriptExtenderSave" region through the save's LSF visitor. The same
// function exists here with no symbol; it was found by the visitor calls
// upstream's pattern keys on (LSFVisitor at +0xB0 here, +0xC0 on Windows,
// the difference being the 16-byte STDString before it).
//
// Serialization logic is bg3se's (Extender/Shared/SavegameSerializer.inl, by
// Norbyte and the bg3se contributors).

#include "savegame.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <mutex>

#include "debug_server.h"
#include "hook.h"
#include "log.h"
#include "lua_host.h"

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" bool bg3le_fixed_string_create(char const* text, std::uint32_t* out);
extern "C" bool bg3le_engine_strings_install();
extern "C" bool bg3le_savegame_read_buffer(void* visitor, int slot,
                                           std::uint32_t name, std::string* out);
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t size);
extern "C" bool bg3le_meta_parse_guid(const char* text, void* out);

namespace bg3le {
namespace {

// esv::OsirisVariableHelper::SavegameVisit(helper, SavegameVisitor*, ?).
constexpr std::uintptr_t kVariableHelperVisit = 0x4199990;
constexpr unsigned char kVariableHelperPrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10,
    0x49, 0x89, 0xfe, 0x48, 0x8b, 0xbe, 0xb0, 0x00, 0x00, 0x00};

constexpr std::size_t kLsfVisitorOffset = 0xb0;

// ObjectVisitor slots in this build. The first twenty match bg3se's
// Serialization.h plus one for the Itanium destructor pair; the typed
// Visit overloads are in declaration order, the reverse of MSVC's grouping.
//
// Upstream's Visit* are one overload set, which MSVC lays out reversed and
// Itanium in declaration order: slot = 77 - bg3se's index. Each typed slot
// was checked against the LSF type code its body passes (Bool 19, UInt8 1,
// Float 6, Int64 32, Double 7, Guid 31).
enum Slot : int {
    kIsReading = 9,
    kEnterRegion = 14,
    kExitRegion = 16,
    kEnterNode = 17,
    kExitNode = 19,
    kVisitCount = 20,
    kVisitBuffer = 21,
    kVisitUInt8 = 23,
    kVisitBool = 24,
    kVisitUInt32 = 27,
    kVisitDouble = 29,
    kVisitFloat = 30,
    kVisitInt64 = 31,
    kVisitFixedString = 51,
    kVisitSTDString = 52,
    kVisitGuid = 54,
};

// Larian's 16-byte string: inline up to 15 chars with the length in the
// last byte, else pointer, size, capacity with the top bit set.
struct RawString {
    unsigned char bytes[16];
};
static_assert(sizeof(RawString) == 16);

RawString make_raw(std::string const& text) {
    RawString raw{};
    if (text.size() <= 15) {
        std::memcpy(raw.bytes, text.data(), text.size());
        raw.bytes[15] = static_cast<unsigned char>(text.size());
    } else {
        char const* p = text.c_str();
        const auto size = static_cast<std::uint32_t>(text.size());
        const std::uint32_t cap = size | 0x80000000u;
        std::memcpy(raw.bytes, &p, 8);
        std::memcpy(raw.bytes + 8, &size, 4);
        std::memcpy(raw.bytes + 12, &cap, 4);
    }
    return raw;
}

// The engine's buffer, if it allocated one, is left alone.
std::string read_raw(RawString const& raw) {
    if ((raw.bytes[15] & 0x80) == 0) {
        return std::string(reinterpret_cast<char const*>(raw.bytes),
                           raw.bytes[15] & 0x0f);
    }
    char const* p = nullptr;
    std::uint32_t size = 0;
    std::memcpy(&p, raw.bytes, 8);
    std::memcpy(&size, raw.bytes + 8, 4);
    return p != nullptr ? std::string(p, size) : std::string();
}

class Visitor {
public:
    explicit Visitor(void* self) : self_(self) {}

    bool IsReading() { return call<bool (*)(void*)>(kIsReading)(self_); }
    bool EnterRegion(std::uint32_t const& name) {
        return call<bool (*)(void*, std::uint32_t const*)>(kEnterRegion)(self_, &name);
    }
    void ExitRegion(std::uint32_t const& name) {
        call<void (*)(void*, std::uint32_t const*)>(kExitRegion)(self_, &name);
    }
    bool EnterNode(std::uint32_t const& name, std::uint32_t const& key) {
        return call<bool (*)(void*, std::uint32_t const*, std::uint32_t const*)>(
            kEnterNode)(self_, &name, &key);
    }
    void ExitNode(std::uint32_t const& name) {
        call<void (*)(void*, std::uint32_t const*)>(kExitNode)(self_, &name);
    }
    void VisitCount(std::uint32_t const& name, std::uint32_t* count) {
        call<void (*)(void*, std::uint32_t const*, std::uint32_t*)>(kVisitCount)(
            self_, &name, count);
    }
    void VisitUInt32(std::uint32_t const& name, std::uint32_t& value,
                     std::uint32_t def) {
        call<void (*)(void*, std::uint32_t const*, std::uint32_t*, std::uint32_t)>(
            kVisitUInt32)(self_, &name, &value, def);
    }
    void VisitFixedString(std::uint32_t const& name, std::uint32_t& value,
                          std::uint32_t const& def) {
        call<void (*)(void*, std::uint32_t const*, std::uint32_t*, std::uint32_t const*)>(
            kVisitFixedString)(self_, &name, &value, &def);
    }
    void VisitSTDString(std::uint32_t const& name, RawString& value,
                        RawString const& def) {
        call<void (*)(void*, std::uint32_t const*, RawString*, RawString const*)>(
            kVisitSTDString)(self_, &name, &value, &def);
    }
    void VisitUInt8(std::uint32_t const& name, std::uint8_t& value, std::uint8_t def) {
        call<void (*)(void*, std::uint32_t const*, std::uint8_t*, std::uint8_t)>(
            kVisitUInt8)(self_, &name, &value, def);
    }
    void VisitBool(std::uint32_t const& name, bool& value, bool def) {
        call<void (*)(void*, std::uint32_t const*, bool*, bool)>(kVisitBool)(
            self_, &name, &value, def);
    }
    void VisitInt64(std::uint32_t const& name, std::int64_t& value, std::int64_t def) {
        call<void (*)(void*, std::uint32_t const*, std::int64_t*, std::int64_t)>(
            kVisitInt64)(self_, &name, &value, def);
    }
    void VisitDouble(std::uint32_t const& name, double& value, double def) {
        call<void (*)(void*, std::uint32_t const*, double*, double)>(kVisitDouble)(
            self_, &name, &value, def);
    }
    void VisitFloat(std::uint32_t const& name, float& value, float def) {
        call<void (*)(void*, std::uint32_t const*, float*, float)>(kVisitFloat)(
            self_, &name, &value, def);
    }
    void VisitGuid(std::uint32_t const& name, std::uint8_t (&value)[16]) {
        static const std::uint8_t kNull[16] = {};
        call<void (*)(void*, std::uint32_t const*, std::uint8_t*, std::uint8_t const*)>(
            kVisitGuid)(self_, &name, value, kNull);
    }
    bool ReadBuffer(std::uint32_t const& name, std::string* out) {
        return bg3le_savegame_read_buffer(self_, kVisitBuffer, name, out);
    }
    void* Self() const { return self_; }

private:
    template <class Fn>
    Fn call(int slot) {
        return reinterpret_cast<Fn>((*reinterpret_cast<void***>(self_))[slot]);
    }
    void* self_;
};

constexpr std::uint32_t kNullString = 0xffffffffu;

// Held for the life of the process, so the reference is never released.
std::uint32_t intern(char const* text) {
    std::uint32_t id = kNullString;
    return bg3le_fixed_string_create(text, &id) ? id : kNullString;
}

struct Names {
    std::uint32_t ScriptExtenderSave, ExtenderVersion, LuaVariables, Mod, ModId,
        Empty, UserVariables, EntityVariables, Entity, Variable, Name, Type,
        Value, ModVariables, Module, PersistentTimers, GameTimers,
        RealtimeTimers, Timer, Time, FrozenTime, Repeat, Paused, Handler, Args;
};

Names const& names() {
    static const Names n{intern("ScriptExtenderSave"), intern("ExtenderVersion"),
                         intern("LuaVariables"), intern("Mod"), intern("ModId"),
                         intern(""), intern("UserVariables"),
                         intern("EntityVariables"), intern("Entity"),
                         intern("Variable"), intern("Name"), intern("Type"),
                         intern("Value"), intern("ModVariables"), intern("Module"),
                         intern("PersistentTimers"), intern("GameTimers"),
                         intern("RealtimeTimers"), intern("Timer"), intern("Time"),
                         intern("FrozenTime"), intern("Repeat"), intern("Paused"),
                         intern("Handler"), intern("Args")};
    return n;
}

constexpr std::uint32_t kSavegameVersion = 12;

// What the last save read held, and whether it has reached the mods yet.
std::mutex g_saved_mutex;
std::vector<std::pair<std::string, std::string>> g_saved;
bool g_restore_pending = false;

std::string fixed_string_text(std::uint32_t id) {
    std::uint32_t length = 0;
    char const* text = bg3le_fixed_string(id, &length);
    return text != nullptr ? std::string(text, length) : std::string();
}

void read_persistent_variables(Visitor& v, Names const& n) {
    std::vector<std::pair<std::string, std::string>> variables;
    std::uint32_t numMods = 0;
    v.VisitCount(n.Mod, &numMods);

    for (std::uint32_t i = 0; i < numMods; ++i) {
        if (!v.EnterNode(n.Mod, n.ModId)) continue;
        std::uint32_t modId = kNullString;
        v.VisitFixedString(n.ModId, modId, n.Empty);
        RawString vars{};
        const RawString nullStr{};
        v.VisitSTDString(n.LuaVariables, vars, nullStr);
        variables.emplace_back(fixed_string_text(modId), read_raw(vars));
        v.ExitNode(n.Mod);
    }

    logf("savegame: read PersistentVars for %zu mod(s)", variables.size());
    {
        const std::lock_guard<std::mutex> lock(g_saved_mutex);
        g_saved = std::move(variables);
        g_restore_pending = true;
    }
    if (debug_server_on_story_thread()) lua_restore_persistent_vars();
}

void write_persistent_variables(Visitor& v, Names const& n) {
    std::vector<std::pair<std::string, std::string>> mods;
    const bool fromLua =
        debug_server_on_story_thread() && lua_persistent_vars_to_save(&mods);
    if (!fromLua) {
        // Upstream's fallback when the Lua state cannot answer.
        const std::lock_guard<std::mutex> lock(g_saved_mutex);
        mods = g_saved;
        for (auto const& mod : mods) {
            logf("Persistent variables for mod %s could not be retrieved, "
                 "saving cached values!", mod.first.c_str());
        }
    }

    const RawString nullStr{};
    for (auto const& [modId, json] : mods) {
        if (!v.EnterNode(n.Mod, n.ModId)) continue;
        std::uint32_t id = intern(modId.c_str());
        v.VisitFixedString(n.ModId, id, n.Empty);
        RawString vars = make_raw(json);
        v.VisitSTDString(n.LuaVariables, vars, nullStr);
        v.ExitNode(n.Mod);
    }
    logf("savegame: wrote PersistentVars for %zu mod(s)", mods.size());
}

// ---- Ext.Vars and persistent timers ----
//
// UserVariable, UserVariableManager, ModVariableMap/Manager and TimerManager
// SavegameVisit, from upstream's UserVariables.inl and Timer.inl.

std::mutex g_extras_mutex;
SaveExtras g_extras;
bool g_extras_pending = false;

std::string guid_text(std::uint8_t const (&guid)[16]) {
    char text[64] = {};
    return bg3le_meta_format_guid(guid, text, sizeof(text)) ? std::string(text)
                                                            : std::string();
}

bool guid_bytes(std::string const& text, std::uint8_t (&guid)[16]) {
    std::memset(guid, 0, sizeof(guid));
    return bg3le_meta_parse_guid(text.c_str(), guid);
}

void visit_value(Visitor& v, Names const& n, SavedVariable& var, bool reading) {
    v.VisitUInt8(n.Type, var.Type, 0);
    const RawString nullStr{};
    switch (var.Type) {
    case 5:
        v.VisitBool(n.Value, var.Bool, false);
        break;
    case 1:
        v.VisitInt64(n.Value, var.Int, 0);
        break;
    case 2:
        v.VisitDouble(n.Value, var.Num, 0.0);
        break;
    case 3: {
        std::uint32_t id = reading ? kNullString : intern(var.Str.c_str());
        v.VisitFixedString(n.Value, id, n.Empty);
        if (reading) var.Str = fixed_string_text(id);
        break;
    }
    case 4: {
        RawString raw = reading ? RawString{} : make_raw(var.Str);
        v.VisitSTDString(n.Value, raw, nullStr);
        if (reading) var.Str = read_raw(raw);
        break;
    }
    case 6:
        // Written only by bg3se; bg3le writes tables as type 4.
        if (reading && !v.ReadBuffer(n.Value, &var.Str)) var.Type = 0;
        break;
    default:
        break;
    }
}

// The Variable nodes of one entity or module.
void read_variables(Visitor& v, Names const& n, std::string const& owner,
                    std::vector<SavedVariable>* out) {
    std::uint32_t count = 0;
    v.VisitCount(n.Variable, &count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!v.EnterNode(n.Variable, n.Name)) continue;
        SavedVariable var;
        var.Owner = owner;
        std::uint32_t name = kNullString;
        v.VisitFixedString(n.Name, name, n.Empty);
        var.Name = fixed_string_text(name);
        visit_value(v, n, var, true);
        if (var.Type != 0) out->push_back(std::move(var));
        v.ExitNode(n.Variable);
    }
}

void write_variables(Visitor& v, Names const& n,
                     std::vector<SavedVariable*> const& vars) {
    for (SavedVariable* var : vars) {
        if (!v.EnterNode(n.Variable, n.Name)) continue;
        std::uint32_t name = intern(var->Name.c_str());
        v.VisitFixedString(n.Name, name, n.Empty);
        visit_value(v, n, *var, false);
        v.ExitNode(n.Variable);
    }
}

// UserVariables > EntityVariables, or ModVariables > ModVariables: an owner
// node per entity or module, keyed by its GUID.
void visit_owned(Visitor& v, Names const& n, std::uint32_t outer,
                 std::uint32_t inner, std::uint32_t key,
                 std::vector<SavedVariable>* vars, bool reading) {
    if (!v.EnterNode(outer, n.Empty)) return;
    if (reading) {
        std::uint32_t owners = 0;
        v.VisitCount(inner, &owners);
        for (std::uint32_t i = 0; i < owners; ++i) {
            if (!v.EnterNode(inner, key)) continue;
            std::uint8_t guid[16] = {};
            v.VisitGuid(key, guid);
            read_variables(v, n, guid_text(guid), vars);
            v.ExitNode(inner);
        }
    } else {
        std::vector<std::pair<std::string, std::vector<SavedVariable*>>> grouped;
        for (SavedVariable& var : *vars) {
            auto it = std::find_if(grouped.begin(), grouped.end(),
                                   [&](auto const& g) { return g.first == var.Owner; });
            if (it == grouped.end()) {
                grouped.emplace_back(var.Owner, std::vector<SavedVariable*>{});
                it = grouped.end() - 1;
            }
            it->second.push_back(&var);
        }
        for (auto& [owner, list] : grouped) {
            std::uint8_t guid[16];
            if (!guid_bytes(owner, guid)) continue;
            if (!v.EnterNode(inner, key)) continue;
            v.VisitGuid(key, guid);
            write_variables(v, n, list);
            v.ExitNode(inner);
        }
    }
    v.ExitNode(outer);
}

void visit_timer(Visitor& v, Names const& n, SavedTimer& t, bool reading) {
    double time = 0.0;  // frozen for the save, as upstream's FreezeBeforeSave
    v.VisitDouble(n.Time, time, 0.0);
    v.VisitFloat(n.FrozenTime, t.Frozen, 0.0f);
    v.VisitFloat(n.Repeat, t.Repeat, 0.0f);
    v.VisitBool(n.Paused, t.Paused, false);
    std::uint32_t handler = reading ? kNullString : intern(t.Handler.c_str());
    v.VisitFixedString(n.Handler, handler, n.Empty);
    const RawString nullStr{};
    RawString args = reading ? RawString{} : make_raw(t.Args);
    v.VisitSTDString(n.Args, args, nullStr);
    if (reading) {
        t.Handler = fixed_string_text(handler);
        t.Args = read_raw(args);
    }
}

void visit_timers(Visitor& v, Names const& n, std::vector<SavedTimer>* timers,
                  bool reading) {
    if (!v.EnterNode(n.PersistentTimers, n.Empty)) return;
    if (v.EnterNode(n.GameTimers, n.Empty)) {
        if (reading) {
            std::uint32_t count = 0;
            v.VisitCount(n.Timer, &count);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!v.EnterNode(n.Timer, n.Empty)) continue;
                SavedTimer t;
                visit_timer(v, n, t, true);
                timers->push_back(std::move(t));
                v.ExitNode(n.Timer);
            }
        } else {
            for (SavedTimer& t : *timers) {
                if (!v.EnterNode(n.Timer, n.Empty)) continue;
                visit_timer(v, n, t, false);
                v.ExitNode(n.Timer);
            }
        }
        v.ExitNode(n.GameTimers);
    }
    // Persistent timers only run on the game clock, upstream as here.
    if (v.EnterNode(n.RealtimeTimers, n.Empty)) v.ExitNode(n.RealtimeTimers);
    v.ExitNode(n.PersistentTimers);
}

void visit_extras(Visitor& v, Names const& n, bool reading) {
    SaveExtras extras;
    if (!reading && !(debug_server_on_story_thread() && lua_extras_to_save(&extras))) {
        // As with PersistentVars: what the last read held, rather than nothing.
        const std::lock_guard<std::mutex> lock(g_extras_mutex);
        extras = g_extras;
    }

    visit_owned(v, n, n.UserVariables, n.EntityVariables, n.Entity, &extras.User,
                reading);
    visit_owned(v, n, n.ModVariables, n.ModVariables, n.Module, &extras.Mod, reading);
    visit_timers(v, n, &extras.Timers, reading);

    logf("savegame: %s %zu user variable(s), %zu mod variable(s), %zu "
         "persistent timer(s)", reading ? "read" : "wrote", extras.User.size(),
         extras.Mod.size(), extras.Timers.size());
    if (reading) {
        {
            const std::lock_guard<std::mutex> lock(g_extras_mutex);
            g_extras = std::move(extras);
            g_extras_pending = true;
        }
        if (debug_server_on_story_thread()) lua_restore_save_extras();
    }
}

// SavegameSerializer::SavegameVisit and SerializePersistentVariables.
void savegame_visit(void* lsf) {
    Names const& n = names();
    if (n.ScriptExtenderSave == kNullString) return;
    Visitor v(lsf);
    if (!v.EnterRegion(n.ScriptExtenderSave)) return;

    std::uint32_t version = kSavegameVersion;
    v.VisitUInt32(n.ExtenderVersion, version, 0);
    const bool reading = v.IsReading();
    if (reading && version > kSavegameVersion) {
        logf("Savegame version too new! Extender version %u, savegame version "
             "%u; savegame data will not be loaded!", kSavegameVersion, version);
    } else {
        if (v.EnterNode(n.LuaVariables, n.Empty)) {
            if (reading) {
                read_persistent_variables(v, n);
            } else {
                write_persistent_variables(v, n);
            }
            v.ExitNode(n.LuaVariables);
        }
        // SavegameVerAddedUserVars (9) and SavegameVerAddedTimers (10).
        if (!reading || version >= 9) visit_extras(v, n, reading);
    }

    v.ExitRegion(n.ScriptExtenderSave);
}

using VisitProc = std::uint64_t (*)(void*, void*, void*);
VisitProc g_original_visit = nullptr;

std::uint64_t visit_hook(void* helper, void* visitor, void* extra) {
    void* lsf = visitor != nullptr
        ? *reinterpret_cast<void**>(static_cast<char*>(visitor) + kLsfVisitorOffset)
        : nullptr;
    if (lsf != nullptr) savegame_visit(lsf);
    return g_original_visit(helper, visitor, extra);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> saved_persistent_vars() {
    const std::lock_guard<std::mutex> lock(g_saved_mutex);
    return g_saved;
}

bool take_saved_extras(SaveExtras* out) {
    const std::lock_guard<std::mutex> lock(g_extras_mutex);
    if (!g_extras_pending) return false;
    g_extras_pending = false;
    *out = g_extras;
    return true;
}

bool take_saved_persistent_vars(
    std::vector<std::pair<std::string, std::string>>* out) {
    const std::lock_guard<std::mutex> lock(g_saved_mutex);
    if (!g_restore_pending) return false;
    g_restore_pending = false;
    *out = g_saved;
    return true;
}

void install_savegame_hook() {
    if (!bg3le_engine_strings_install()) {
        logf("savegame: no FixedString::CreateFromString; PersistentVars "
             "will not be saved");
        return;
    }
    if (!bytes_match(kVariableHelperVisit, kVariableHelperPrologue,
                     sizeof(kVariableHelperPrologue))) {
        logf("savegame: OsirisVariableHelper::SavegameVisit not at %#lx; "
             "PersistentVars will not be saved",
             (unsigned long)kVariableHelperVisit);
        return;
    }
    void* original = nullptr;
    if (hook_call_sites(kVariableHelperVisit,
                        reinterpret_cast<void*>(&visit_hook), &original) == 0) {
        logf("savegame: no call sites patched; PersistentVars will not be saved");
        return;
    }
    g_original_visit = reinterpret_cast<VisitProc>(original);
}

}  // namespace bg3le
