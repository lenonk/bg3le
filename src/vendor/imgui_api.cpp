// Ext.IMGUI's C side: creating widgets and reaching them by handle.
//
// bg3se's widget tree is an IMGUIObjectManager holding pools of Renderable,
// each addressed by a 64-bit handle rather than a pointer, and every widget
// type has a property map that bg3le already re-expands into its own field
// tables. So a widget needs no wrapper object here: the handle is the
// identity, the field machinery reads and writes the properties, and the
// only thing this file adds is the Add* methods, which are P_FUN entries and
// so are deliberately absent from the field tables.
//
// The manager the widgets live in is created here too. IMGUIManager takes one
// through SetObjects and nothing was giving it one, so even with the render
// backend up there was nowhere for a window to be.
//
// Callbacks are not wired yet, and that is the honest boundary of this first
// piece: bg3se delivers them through lua::DeferredLuaDelegateQueue against
// its own Lua state, which bg3le does not have, so a button draws and does
// nothing when clicked. Reaching them needs bg3le's own delivery, which is
// the next piece rather than this one.
//
// IMGUIObjectManager, Renderable and every widget type are by Norbyte and the
// bg3se contributors (https://github.com/Norbyte/bg3se); the binding is ours.

#include <stdafx.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/IMGUI/Objects.h>
#include <Extender/ScriptExtender.h>

#include "imgui_args.h"

#include <cstring>
#include <memory>
#include <mutex>

#include "../log.h"

namespace bg3le {

// src/vendor/imgui_methods.cpp, which is where the methods themselves live.
std::uint64_t imgui_add_child(bg3se::extui::TreeParent* parent,
                              char const* kind, ImguiArg const* args,
                              std::size_t count);
bool imgui_call_method(bg3se::extui::Renderable* object, char const* name,
                       ImguiArg const* args, std::size_t count,
                       ImguiArg* out);
std::size_t imgui_children(bg3se::extui::Renderable* object,
                           std::uint64_t* out, std::size_t capacity);

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

std::unique_ptr<bg3se::extui::IMGUIObjectManager>& objects() {
    static std::unique_ptr<bg3se::extui::IMGUIObjectManager> manager;
    return manager;
}

bg3se::extui::IMGUIManager* ui() {
    if (bg3se::gExtender == nullptr) return nullptr;
    return &bg3se::gExtender->IMGUI();
}

bg3se::extui::Renderable* renderable(std::uint64_t handle) {
    auto& manager = objects();
    if (manager == nullptr) return nullptr;
    return manager->GetRenderable(handle);
}

}  // namespace

// Creates the widget tree the manager draws from. Called once the render
// backend is up.
void imgui_api_init() {
    const std::lock_guard<std::mutex> held(lock());
    if (objects() != nullptr) return;

    auto* manager = ui();
    if (manager == nullptr) return;

    objects() = std::make_unique<bg3se::extui::IMGUIObjectManager>();
    manager->SetObjects(objects().get());
    logf("imgui: widget tree created and handed to the manager");
}

std::mutex& imgui_frame_mutex();

// A fresh widget tree, when the Lua states are rebuilt: upstream's belongs to
// the client state and goes with it, so a mod building its window again does
// not find the old one still there.
void imgui_api_reset() {
    const std::lock_guard<std::mutex> frame(imgui_frame_mutex());
    const std::lock_guard<std::mutex> held(lock());
    auto* manager = ui();
    if (manager == nullptr || objects() == nullptr) return;
    manager->SetObjects(nullptr);
    objects() = std::make_unique<bg3se::extui::IMGUIObjectManager>();
    manager->SetObjects(objects().get());
    logf("imgui: widget tree replaced for the new Lua states");
}

// Hands the queued callbacks to bg3le's own delivery.
//
// Upstream flushes this from IMGUIObjectManager::ClientUpdate, but only
// after pinning its own client Lua state -- and bg3le never attaches one, so
// that pin is always false and nothing was ever flushed: every click and
// every change sat in the queue for the life of the process. The queue is
// drained here instead, on the thread that just drew the frame.
//
// The lua_State* is unused: bg3le's LuaDelegate posts to its own queue
// rather than calling through a registry entry, and that queue knows which
// context registered each callback. See src/vendor/imgui_events.cpp.
void imgui_flush_events() {
    const std::lock_guard<std::mutex> held(lock());
    auto& manager = objects();
    if (manager == nullptr) return;
    manager->GetEventQueue().Flush(nullptr);
}

// The widget a handle names, and bg3se's own short name for its class.
// src/vendor/imgui_events.cpp needs both to find where an event lives.
bg3se::extui::Renderable* imgui_renderable(std::uint64_t handle) {
    const std::lock_guard<std::mutex> held(lock());
    return renderable(handle);
}

char const* imgui_type_name(std::uint64_t handle) {
    const std::lock_guard<std::mutex> held(lock());
    auto* object = renderable(handle);
    return object != nullptr ? object->GetTypeName() : nullptr;
}

}  // namespace bg3le

// ---- the entry points Ext.IMGUI is built on -------------------------------

extern "C" std::uint64_t bg3le_imgui_new_window(char const* name) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto& manager = bg3le::objects();
    if (manager == nullptr || name == nullptr) {
        return bg3se::extui::InvalidHandle;
    }

    auto* window = manager->CreateRenderable<bg3se::extui::Window>();
    if (window == nullptr) return bg3se::extui::InvalidHandle;

    window->Label = name;
    window->Open = true;
    return window->Handle;
}

extern "C" std::uint64_t bg3le_imgui_add(std::uint64_t parent,
                                         char const* kind,
                                         ImguiArg const* args,
                                         std::size_t count) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    if (kind == nullptr) return bg3se::extui::InvalidHandle;

    auto* object = bg3le::renderable(parent);
    if (object == nullptr) return bg3se::extui::InvalidHandle;

    // Only a widget that can have children, which is what TreeParent means.
    auto* tree = dynamic_cast<bg3se::extui::TreeParent*>(object);
    if (tree == nullptr) return bg3se::extui::InvalidHandle;

    return bg3le::imgui_add_child(tree, kind, args, count);
}

// Calls a method by name. False if the widget has no such method, which is
// not the same as a method that returned nothing.
extern "C" bool bg3le_imgui_call(std::uint64_t handle, char const* name,
                                 ImguiArg const* args, std::size_t count,
                                 ImguiArg* out) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    if (name == nullptr || out == nullptr) return false;

    auto* object = bg3le::renderable(handle);
    if (object == nullptr) return false;

    return bg3le::imgui_call_method(object, name, args, count, out);
}

// A container's children. Returns the total, writing as many as fit.
extern "C" std::size_t bg3le_imgui_children(std::uint64_t handle,
                                            std::uint64_t* out,
                                            std::size_t capacity) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto* object = bg3le::renderable(handle);
    if (object == nullptr) return 0;
    return bg3le::imgui_children(object, out, capacity);
}

// The address and class of a widget, so the field machinery can read and
// write its properties the way it does a component's.
extern "C" void* bg3le_imgui_object(std::uint64_t handle,
                                    char const** typeName) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto* object = bg3le::renderable(handle);
    if (object == nullptr) return nullptr;
    if (typeName != nullptr) *typeName = object->GetTypeName();
    return object;
}

extern "C" bool bg3le_imgui_destroy(std::uint64_t handle) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto& manager = bg3le::objects();
    if (manager == nullptr) return false;
    return manager->DestroyRenderable(handle);
}

extern "C" void bg3le_imgui_enable_demo(bool enabled) {
    const std::lock_guard<std::mutex> held(bg3le::lock());
    auto& manager = bg3le::objects();
    if (manager != nullptr) manager->EnableDemo(enabled);
}
