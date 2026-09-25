// The two bg3se globals its own UI code reaches through, stood up for bg3le.
//
// bg3se's ImGui overlay is compiled into libbg3le.so and its hooks install
// (see src/detour_interpose.cpp), but IMGUIManager::InitializeUI faulted
// immediately on `gExtender->GetConfig()` -- bg3le never creates a
// ScriptExtender, so gExtender is null -- and would have faulted again on
// `GetStaticSymbols()`, which dereferences a null gStaticSymbols.
//
// Neither needs much. What the overlay actually reads is small and, apart
// from the manager itself, cosmetic:
//
//   gExtender->IMGUI()                        the manager -- this matters
//   gExtender->GetConfig().DeveloperMode      two imgui debug flags
//   GetStaticSymbols().ToPath("imgui.ini")    where imgui saves window
//                                             positions; returns "" without
//                                             path roots, which upstream's
//                                             own code already handles
//   GetGlobalSwitches()->Language             whether to drop the smallest
//                                             font sizes, for CJK glyphs
//
// The manager is the reason a real ScriptExtender is constructed rather than
// a stub: the widget code reaches it as gExtender->IMGUI(), so the manager
// bg3le drives and the manager the widgets register textures and fonts with
// have to be the same object. Constructing one is cheap -- its members'
// constructors are empty and it allocates a console -- and nothing calls
// Initialize() or PostStartup(), which are where upstream's real work is.
//
// GlobalSwitches is bg3le's own default until the engine's is located, and
// that is deliberate rather than a placeholder: the only thing read from it
// here is the language, to decide a font size. Answering that from a default
// is honest; pointing it at an object found on a layout that does not match
// this build would not be. See reference/GLOBAL-SWITCHES.md, and
// bg3le_set_global_switches below for where the real one goes when it is
// found.
//
// ScriptExtender, StaticSymbols and GlobalSwitches are by Norbyte and the
// bg3se contributors (https://github.com/Norbyte/bg3se); standing them up
// here is ours.

#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Extender/Client/ScriptExtenderClient.h>
#include <GameDefinitions/Symbols.h>

#include <memory>
#include <mutex>

#include "../hook.h"
#include "../log.h"
#include "../mem.h"
#include <cstring>

extern "C" bool bg3le_settings_flag(char const* key, bool fallback);

// ls::ThreadRegistry::RequestThreadIndex, which this build inlines: the
// index lives in a thread-local int at fs:-0x24d28, as the FixedString
// lookup at image+0x2b82497 reads it (checked before use), and an engine
// thread has one by the time it runs Lua. -1 for a thread without one; the
// engine's own registration also installs a thread-exit hook, so bg3le does
// not register threads itself.
extern "C" int bg3le_engine_thread_index() {
    static int usable = -1;
    if (usable < 0) {
        constexpr unsigned char kRead[] = {0x64, 0x44, 0x8b, 0x34, 0x25, 0xd8, 0xb2, 0xfd, 0xff};
        unsigned char held[sizeof(kRead)] = {};
        usable = bg3le::safe_read((void const*)(bg3le::load_bias() + 0x2b82497), held, sizeof(held))
                     && std::memcmp(held, kRead, sizeof(kRead)) == 0;
        if (!usable) bg3le::logf("threads: the thread-index read is not where this build has it");
    }
    if (!usable) return -1;
    std::uintptr_t tp = 0;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    std::int32_t index = -1;
    std::memcpy(&index, (void const*)(tp - 0x24d28), sizeof(index));
    return index;
}

static std::uint32_t request_thread_index() {
    const int index = bg3le_engine_thread_index();
    return index < 0 ? 0 : (std::uint32_t)index;
}

namespace bg3le {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

bool g_ready = false;

// What GetGlobalSwitches() hands back. Starts as bg3le's own default and is
// repointed if the engine's is ever confirmed.
bg3se::GlobalSwitches* g_switches = nullptr;

bg3se::GlobalSwitches& default_switches() {
    static bg3se::GlobalSwitches fallback{};
    return fallback;
}

}  // namespace

// Creates gExtender and gStaticSymbols if they are not there yet.
//
// Safe to call more than once and from either context; the overlay calls it
// before it builds anything.
void extender_globals_init() {
    const std::lock_guard<std::mutex> held(lock());
    if (g_ready) return;
    g_ready = true;

    if (bg3se::gStaticSymbols == nullptr) {
        // Members bg3le has not located stay null; each accessor checks.
        bg3se::gStaticSymbols = new bg3se::StaticSymbols();
    }

    if (g_switches == nullptr) g_switches = &default_switches();
    bg3se::gStaticSymbols->ls__GlobalSwitches = &g_switches;

    // ls::PathRoots: STDString*[19] by PathRootType, found by content in a
    // live game (Public at [2], Projects at [9], the rest in enum order).
    // Upstream's ToPath checks each entry, so one the engine has not filled
    // yet reads as unset rather than as garbage.
    constexpr std::uintptr_t kPathRoots = 0x7d9cd60;
    bg3se::gStaticSymbols->ls__ThreadRegistry__RequestThreadIndex = &request_thread_index;
    bg3se::gStaticSymbols->ls__PathRoots =
        reinterpret_cast<bg3se::STDString**>(bg3le::load_bias() + kPathRoots);

    // ls::gTextureAtlasMap: found by content (46 atlases keyed by their .lsx
    // paths, 7089 icons); IconMap is what ImageReference::BindIcon reads.
    constexpr std::uintptr_t kTextureAtlasMap = 0x7d1c438;
    bg3se::gStaticSymbols->ls__gTextureAtlasMap =
        reinterpret_cast<bg3se::TextureAtlasMap**>(bg3le::load_bias()
                                                   + kTextureAtlasMap);

    if (bg3se::gExtender == nullptr) {
        bg3se::gExtender = std::make_unique<bg3se::ScriptExtender>();
    }

    // And the client's extension state, because the ImGui manager's own
    // update reaches for it every frame.
    //
    // IMGUIObjectManager::ClientUpdate does one thing -- pin the client Lua
    // state and flush the deferred callback queue -- and it gets the state
    // through ecl::ExtensionState::Get(), which asserts on a null
    // unique_ptr. That assert traps, so the first frame after a widget tree
    // was attached killed the game with SIGILL rather than an error.
    //
    // ResetExtensionState is what fills it in upstream, and all three things
    // it does are trivial here: clear a list of local messages, seed an RNG,
    // clear the path overrides. With the state present the pin finds no Lua
    // -- bg3le's contexts are its own -- so the flush is skipped, which is
    // the right answer until bg3le delivers those callbacks itself.
    bg3se::gExtender->GetClient().ResetExtensionState();
    // Upstream reads it from ScriptExtenderSettings.json; off by default.
    bg3se::gExtender->GetConfig().DeveloperMode =
        bg3le_settings_flag("DeveloperMode", false);

    logf("extender: globals stood up (config, static symbols and the client "
         "extension state; global switches are bg3le's default until the "
         "engine's is confirmed)");
}

// Points GetGlobalSwitches() at the engine's own object.
//
// Nothing calls this yet -- the search in src/vendor/global_switches.cpp
// refuses, because bg3se's declared layout is not this build's. It is here so
// that locating the object is the only work left, rather than locating it and
// then finding out where it has to be plumbed.
void extender_set_global_switches(void* engineSwitches) {
    const std::lock_guard<std::mutex> held(lock());
    g_switches = engineSwitches != nullptr
                     ? (bg3se::GlobalSwitches*)engineSwitches
                     : &default_switches();
    logf("extender: global switches now %s",
         engineSwitches != nullptr ? "the engine's" : "bg3le's default");
}

}  // namespace bg3le

// Ext.StaticData's icon atlases, as upstream's GetTextureAtlasManager,
// GetIconAtlas and GetIconUVs read ls::gTextureAtlasMap. Null when the map
// is not up yet or the icon is not in it.
namespace {
bg3se::TextureAtlasMap* atlas_map() {
    auto** slot = bg3se::gStaticSymbols != nullptr
                      ? bg3se::gStaticSymbols->ls__gTextureAtlasMap : nullptr;
    return slot != nullptr ? *slot : nullptr;
}
}  // namespace

extern "C" void* bg3le_texture_atlas_map() { return atlas_map(); }

extern "C" void* bg3le_icon_atlas(char const* icon) {
    auto* map = atlas_map();
    if (map == nullptr || icon == nullptr) return nullptr;
    return map->IconMap.get_or_default(bg3se::FixedString(icon));
}

extern "C" void* bg3le_icon_uvs(char const* icon) {
    auto* atlas = static_cast<bg3se::TextureAtlas*>(bg3le_icon_atlas(icon));
    if (atlas == nullptr) return nullptr;
    return atlas->Icons.get_or_default(bg3se::FixedString(icon));
}

