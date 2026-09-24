// PersistentVars in the savegame, as bg3se's SavegameSerializer writes them.
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
#include <cstring>
#include <mutex>

#include "debug_server.h"
#include "hook.h"
#include "log.h"
#include "lua_host.h"

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace bg3le {
namespace {

// esv::OsirisVariableHelper::SavegameVisit(helper, SavegameVisitor*, ?).
constexpr std::uintptr_t kVariableHelperVisit = 0x4199990;
constexpr unsigned char kVariableHelperPrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10,
    0x49, 0x89, 0xfe, 0x48, 0x8b, 0xbe, 0xb0, 0x00, 0x00, 0x00};

constexpr std::size_t kLsfVisitorOffset = 0xb0;

// ls::FixedString::CreateFromString(LSStringView const&), found through
// upstream's anchor string; checked by its prologue and its hash seed.
constexpr std::uintptr_t kFixedStringCreate = 0x226db50;
constexpr unsigned char kFixedStringCreatePrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
    0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFixedStringCreateSeedAt = 0x226dbbc;
constexpr unsigned char kFixedStringCreateSeed[] = {0x41, 0xba, 0xed, 0x5e,
                                                    0xad, 0xde};

struct StringView {
    char const* data;
    std::uint32_t size;
};
using CreateProc = std::uint32_t (*)(StringView const*);
CreateProc g_create = nullptr;

// ObjectVisitor slots in this build. The first twenty match bg3se's
// Serialization.h plus one for the Itanium destructor pair; the typed
// Visit overloads are in declaration order, the reverse of MSVC's grouping.
enum Slot : int {
    kIsReading = 9,
    kEnterRegion = 14,
    kExitRegion = 16,
    kEnterNode = 17,
    kExitNode = 19,
    kVisitCount = 20,
    kVisitUInt32 = 27,
    kVisitFixedString = 51,
    kVisitSTDString = 52,
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
    if (g_create == nullptr) return kNullString;
    const StringView view{text, static_cast<std::uint32_t>(std::strlen(text))};
    return g_create(&view);
}

struct Names {
    std::uint32_t ScriptExtenderSave, ExtenderVersion, LuaVariables, Mod, ModId,
        Empty;
};

Names const& names() {
    static const Names n{intern("ScriptExtenderSave"), intern("ExtenderVersion"),
                         intern("LuaVariables"), intern("Mod"), intern("ModId"),
                         intern("")};
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

// SavegameSerializer::SavegameVisit and SerializePersistentVariables.
void savegame_visit(void* lsf) {
    if (g_create == nullptr) return;
    Names const& n = names();
    Visitor v(lsf);
    if (!v.EnterRegion(n.ScriptExtenderSave)) return;

    std::uint32_t version = kSavegameVersion;
    v.VisitUInt32(n.ExtenderVersion, version, 0);
    const bool reading = v.IsReading();
    if (reading && version > kSavegameVersion) {
        logf("Savegame version too new! Extender version %u, savegame version "
             "%u; savegame data will not be loaded!", kSavegameVersion, version);
    } else if (v.EnterNode(n.LuaVariables, n.Empty)) {
        if (reading) {
            read_persistent_variables(v, n);
        } else {
            write_persistent_variables(v, n);
        }
        v.ExitNode(n.LuaVariables);
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

bool take_saved_persistent_vars(
    std::vector<std::pair<std::string, std::string>>* out) {
    const std::lock_guard<std::mutex> lock(g_saved_mutex);
    if (!g_restore_pending) return false;
    g_restore_pending = false;
    *out = g_saved;
    return true;
}

void install_savegame_hook() {
    if (!bytes_match(kFixedStringCreate, kFixedStringCreatePrologue,
                     sizeof(kFixedStringCreatePrologue)) ||
        !bytes_match(kFixedStringCreateSeedAt, kFixedStringCreateSeed,
                     sizeof(kFixedStringCreateSeed))) {
        logf("savegame: FixedString::CreateFromString not at %#lx; "
             "PersistentVars will not be saved",
             (unsigned long)kFixedStringCreate);
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
    g_create = reinterpret_cast<CreateProc>(load_bias() + kFixedStringCreate);
}

}  // namespace bg3le
