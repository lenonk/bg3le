// Drives bg3se's ImGui overlay.
//
// Everything the overlay needs is already in libbg3le.so: bg3se's
// IMGUIManager, its Vulkan backend, imgui itself and bg3le's field tables for
// every widget type. The one thing it could not do was install its hooks,
// because it wraps seven Vulkan entry points through Detours. It can now --
// see src/detour_interpose.cpp and src/vulkan_forward.cpp -- so this is the
// part that was missing: something to construct the manager and turn it on.
//
// On by default, as upstream's is; BG3LE_IMGUI=0 turns it off. The hooks
// sit on vkCreateInstance and vkQueuePresentKHR and the overlay does real
// Vulkan work inside the present call.
//
// EnableHooks has to run before the game creates its Vulkan instance, and
// IMGUIManager's own containers allocate through the engine's heap -- which is
// not installed until bg3le has read the symbol table, 0.4s after the library
// loads. Constructing in the library constructor threw bad_array_new_length
// every time.
//
// So it is built at the first vkCreateInstance instead. That is after the
// allocator and before the instance exists, which is the window both
// requirements leave, and src/vulkan_forward.cpp calls this from there.
//
// IMGUIManager, VulkanBackend and the widgets are by Norbyte and the bg3se
// contributors (https://github.com/Norbyte/bg3se); only the driving is ours.

#include <stdafx.h>

#include <imgui_internal.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/SDLManager.h>
#include <Extender/ScriptExtender.h>

#include <atomic>
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstdlib>
#include <exception>
#include <memory>

#include "../log.h"

extern "C" bool bg3le_game_allocator_ready();

namespace bg3le {

namespace {

// The manager belongs to the extender object, not to this file.
//
// bg3se's widget code reaches it as gExtender->IMGUI() -- that is how a
// texture or a font gets registered -- so driving a separate instance would
// mean two managers, one drawing and one being written to. This drives the
// one the widgets can see.
bool g_started = false;
bool g_waiting_said = false;

bg3se::extui::IMGUIManager* manager() {
    if (bg3se::gExtender == nullptr) return nullptr;
    return &bg3se::gExtender->IMGUI();
}

// Held while a frame is built and while a font is added: upstream does both
// on the client thread, here Lua and the frame run on different threads,
// and a font added mid-frame left the window on the default font.
std::mutex& frame_lock() {
    static std::mutex m;
    return m;
}

}  // namespace

// For src/vendor/imgui_api.cpp, which replaces the widget tree on a reset.
std::mutex& imgui_frame_mutex() { return frame_lock(); }

bool imgui_overlay_wanted();
void extender_globals_init();
void imgui_api_init();
void imgui_flush_events();

// Constructs the manager and installs the hooks. Safe to call more than once,
// and deliberately does not latch until it has actually built something: the
// first calls arrive before the engine heap is installed, and giving up on
// those would mean never starting.
void imgui_overlay_start() {
    if (g_started || !imgui_overlay_wanted()) return;

    if (!bg3le_game_allocator_ready()) {
        if (!g_waiting_said) {
            g_waiting_said = true;
            logf("imgui: waiting for the engine heap before building the "
                 "overlay");
        }
        return;
    }
    g_started = true;

    try {
        extender_globals_init();
        auto* ui = manager();
        if (ui == nullptr) {
            logf("imgui: no extender object; the overlay cannot start");
            return;
        }
        ui->EnableHooks();
        ui->EnableUI(true);
        imgui_api_init();
        logf("imgui: overlay hooks installed; waiting for the swapchain");
    } catch (std::exception const& e) {
        logf("imgui: could not start the overlay: %s", e.what());
    } catch (...) {
        logf("imgui: could not start the overlay");
    }
}

// What the last frame drew, so a test can tell a widget tree that renders
// from one that is merely attached. Headless has no screen to look at.
std::atomic<std::uint64_t> g_frames{0};
std::atomic<int> g_last_vertices{0};
std::atomic<int> g_last_lists{0};
std::atomic<int> g_imgui_frames{0};
std::atomic<float> g_mouse_x{0};
std::atomic<float> g_mouse_y{0};
std::atomic<float> g_display_w{0};
std::atomic<float> g_display_h{0};
std::atomic<bool> g_mouse_down{false};
std::atomic<bool> g_want_mouse{false};
std::atomic<bool> g_hovered_window{false};
std::atomic<bool> g_nav_no_hover{false};

// A window's layout, captured on the thread that drew it.
//
// Read live from the console thread this raced the frame and eventually
// crashed the game: End() restores the window's cursor state and
// FindWindowByName walks a list the drawing thread is rebuilding. Nothing
// here is worth a crash, so the frame takes the snapshot and the console
// reads the copy.
constexpr std::size_t kGeometrySlots = 33;

std::mutex& watch_lock() {
    static std::mutex m;
    return m;
}

std::string g_watch_name;
float g_watch[kGeometrySlots] = {};
bool g_watch_valid = false;
char g_hovered_name[128] = {};
unsigned g_hovered_id = 0;
unsigned g_active_id = 0;
unsigned g_nav_id = 0;
unsigned g_watch_item_id = 0;
std::string g_watch_item;

void record_window() {
    std::string name;
    std::string item;
    {
        const std::lock_guard<std::mutex> held(watch_lock());
        name = g_watch_name;
        item = g_watch_item;
    }

    auto* gui = ImGui::GetCurrentContext();
    std::snprintf(g_hovered_name, sizeof(g_hovered_name), "%s",
                  gui->HoveredWindow != nullptr ? gui->HoveredWindow->Name
                                                : "<none>");
    g_hovered_id = gui->HoveredId;
    g_active_id = gui->ActiveId;
    g_nav_id = gui->NavId;

    if (name.empty()) return;
    auto* window = ImGui::FindWindowByName(name.c_str());
    if (window == nullptr) {
        const std::lock_guard<std::mutex> held(watch_lock());
        g_watch_valid = false;
        return;
    }

    float out[kGeometrySlots] = {};
    out[0] = window->Pos.x;
    out[1] = window->Pos.y;
    out[2] = window->Size.x;
    out[3] = window->Size.y;
    out[4] = window->ContentRegionRect.Min.x;
    out[5] = window->ContentRegionRect.Min.y;
    out[6] = window->ContentRegionRect.Max.x;
    out[7] = window->ContentRegionRect.Max.y;
    out[8] = ImGui::GetIO().FontDefault != nullptr
                 ? ImGui::GetIO().FontDefault->FontSize
                 : 0.0f;
    out[9] = window->DC.CursorStartPos.x;
    out[10] = window->DC.CursorStartPos.y;
    out[11] = window->DC.CursorMaxPos.x;
    out[12] = window->DC.CursorMaxPos.y;
    out[13] = ImGui::GetFontSize();
    out[14] = ImGui::GetIO().FontGlobalScale;
    out[15] = ImGui::GetIO().FontDefault != nullptr
                  ? ImGui::GetIO().FontDefault->Scale
                  : -1.0f;
    out[16] = window->FontWindowScale;
    out[17] = ImGui::GetStyle().FramePadding.y;
    out[18] = (float)ImGui::GetIO().Fonts->Fonts.Size;
    out[19] = window->Collapsed ? 1.0f : 0.0f;
    out[20] = window->SkipItems ? 1.0f : 0.0f;
    out[21] = window->DC.PrevLineSize.y;
    out[22] = window->DC.CursorPos.y;
    out[23] = window->ContentSize.y;
    out[24] = window->ClipRect.Min.x;
    out[25] = window->ClipRect.Min.y;
    out[26] = window->ClipRect.Max.x;
    out[27] = window->ClipRect.Max.y;
    out[28] = window->Hidden ? 1.0f : 0.0f;
    out[29] = (float)window->HiddenFramesCannotSkipItems;
    out[30] = (float)window->HiddenFramesCanSkipItems;
    out[31] = window->InnerClipRect.Min.y;
    out[32] = window->InnerClipRect.Max.y;

    const unsigned itemId = item.empty() ? 0u : window->GetID(item.c_str());

    // Logged from here rather than read back, because this is the thread
    // that drew it and the numbers that matter are the ones the frame used.
    static int said = 0;
    if (said < 3) {
        ++said;
        logf("imgui watch: %s at %.0f,%.0f %.0fx%.0f  clip %.0f,%.0f..%.0f,%.0f"
             "  innerClip y %.0f..%.0f  hidden %d/%d/%d  collapsed %d"
             "  skipItems %d  font %.2f  prevLine %.2f  item %u",
             name.c_str(), out[0], out[1], out[2], out[3], out[24], out[25],
             out[26], out[27], out[31], out[32], window->Hidden ? 1 : 0,
             (int)window->HiddenFramesCannotSkipItems,
             (int)window->HiddenFramesCanSkipItems,
             window->Collapsed ? 1 : 0, window->SkipItems ? 1 : 0, out[13],
             out[21], itemId);
    }

    const std::lock_guard<std::mutex> held(watch_lock());
    std::memcpy(g_watch, out, sizeof(out));
    g_watch_item_id = itemId;
    g_watch_valid = true;
}

void record_frame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);

    // Nothing below is safe without a context: ImGui::GetDrawData reaches
    // through the global one, and the first ticks arrive long before it
    // exists now that this runs from the game's own event loop rather than
    // from the story thread.
    if (ImGui::GetCurrentContext() == nullptr) return;

    // imgui's own count, which only advances if Update got past its early
    // return and reached NewFrame. Without it there is no telling a tick
    // that drew nothing from one that never drew.
    {
        g_imgui_frames.store(ImGui::GetFrameCount(),
                             std::memory_order_relaxed);
    }

    // Where imgui thinks the mouse is, and how big it thinks the screen is.
    // Both are the SDL side's to supply, and both were zero while that was a
    // stub -- worth reporting rather than inferring from a vertex count.
    {
        auto const& io = ImGui::GetIO();
        g_mouse_x.store(io.MousePos.x, std::memory_order_relaxed);
        g_mouse_y.store(io.MousePos.y, std::memory_order_relaxed);
        g_display_w.store(io.DisplaySize.x, std::memory_order_relaxed);
        g_display_h.store(io.DisplaySize.y, std::memory_order_relaxed);
        g_mouse_down.store(io.MouseDown[0], std::memory_order_relaxed);

        // Whether imgui thinks the pointer is over one of its windows, and
        // whether it has turned mouse hovering off because navigation is
        // being driven from the keyboard. Between them they say why an
        // item that is plainly under the cursor does not report as hovered.
        g_want_mouse.store(io.WantCaptureMouse, std::memory_order_relaxed);
        auto* gui = ImGui::GetCurrentContext();
        g_hovered_window.store(gui->HoveredWindow != nullptr,
                               std::memory_order_relaxed);
        g_nav_no_hover.store(gui->NavHighlightItemUnderNav,
                             std::memory_order_relaxed);
    }

    record_window();

    auto* draw = ImGui::GetDrawData();
    if (draw == nullptr) return;
    g_last_vertices.store(draw->TotalVtxCount, std::memory_order_relaxed);
    g_last_lists.store(draw->CmdListsCount, std::memory_order_relaxed);
}

// Mouse input handed straight to imgui, for driving the overlay without a
// mouse.
//
// Not the same thing as SDLManager::InjectEvent, which feeds the game's own
// event loop and deliberately bypasses imgui. These are applied on the
// render thread, immediately before the frame that will see them, which is
// the only thread allowed to touch imgui's IO.
// One button change, at a position. Applied one per frame, because imgui
// trickles its own event queue for exactly this reason: two button changes
// in one frame are one click as far as a widget is concerned, so a sweep
// that queued them all would register once.
struct MouseInput {
    float X{0};
    float Y{0};
    int Button{0};
    bool Down{false};
};

std::mutex& input_lock() {
    static std::mutex m;
    return m;
}

std::deque<MouseInput>& pending_input() {
    static std::deque<MouseInput> queue;
    return queue;
}

// Where the mouse is held, if it is. Sticky rather than one event, because
// the SDL backend sets the position every frame from the real mouse and
// would otherwise put it back.
bool g_holding = false;
float g_hold_x = 0;
float g_hold_y = 0;

// Applied from SDLManager::NewFrame, after the SDL backend has had its say.
//
// Order matters and cost a diagnosis: queued before the backend, an injected
// position is simply overwritten by whatever the real mouse is doing, and
// nothing registers as hovered. After it, these are the last word on the
// frame.
void apply_input() {
    bool holding = false;
    float holdX = 0;
    float holdY = 0;
    bool haveInput = false;
    MouseInput input;
    {
        const std::lock_guard<std::mutex> held(input_lock());
        holding = g_holding;
        holdX = g_hold_x;
        holdY = g_hold_y;
        if (!pending_input().empty()) {
            input = pending_input().front();
            pending_input().pop_front();
            haveInput = true;

            // A button change carries its own position, and it becomes the
            // held one: a widget reads the mouse where it was left.
            g_holding = true;
            g_hold_x = input.X;
            g_hold_y = input.Y;
            holding = true;
            holdX = input.X;
            holdY = input.Y;
        }
    }

    if (!holding && !haveInput) return;

    auto& io = ImGui::GetIO();

    // imgui ignores input to an unfocused application, and a headless run
    // has no focus of its own to give it.
    io.AddFocusEvent(true);

    if (holding) io.AddMousePosEvent(holdX, holdY);
    if (haveInput) io.AddMouseButtonEvent(input.Button, input.Down);
}

void imgui_apply_injected_input() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    apply_input();
}

// Per frame, from the same hook that runs the Lua timers. The backend draws
// from the present hook; this is the manager's own bookkeeping.
void imgui_overlay_tick() {
    if (!g_started) return;
    auto* ui = manager();
    if (ui == nullptr) return;

    // Update calls back into SDL, and this is called from inside
    // SDL_PollEvent; anything in there that pumped the event loop would
    // re-enter and draw a frame inside a frame.
    static thread_local bool inside = false;
    if (inside) return;
    inside = true;
    struct Leave {
        bool* Flag;
        ~Leave() { *Flag = false; }
    } leave{&inside};

    try {
        const std::lock_guard<std::mutex> held(frame_lock());
        ui->Update();
        imgui_flush_events();
        record_frame();
    } catch (std::exception const& e) {
        logf("imgui: Update failed, stopping the overlay: %s", e.what());
        g_started = false;
    } catch (...) {
        logf("imgui: Update failed, stopping the overlay");
        g_started = false;
    }
}

// Whether the backend has got as far as building its own resources, which is
// the milestone worth reporting: it means the hooks fired and the swapchain
// was recognised.
bool imgui_overlay_ready() {
    auto* ui = manager();
    return ui != nullptr && ui->WasUIInitialized();
}

// Whether the overlay was asked for, got as far as installing its hooks, and
// whether the render backend has built its own resources.
//
// Exposed because the alternative is guessing: bg3se's own IMGUI_DEBUG
// logging is compiled out, so between "hooks installed" and a window
// appearing there is nothing in the log at all.
// The four of upstream's Ext.IMGUI functions that are the manager's own.
extern "C" bool bg3le_imgui_load_font(char const* name, char const* path,
                                      float size) {
    auto* ui = bg3le::manager();
    if (ui == nullptr || name == nullptr) return false;
    if (!ui->WasUIInitialized()) return false;
    const std::lock_guard<std::mutex> held(bg3le::frame_lock());
    return ui->LoadFont(bg3se::FixedString(name),
                        path != nullptr ? path : "", size);
}

// Whether a font name resolves, and to a loaded ImGui font: 0 missing,
// 1 known but not loaded, 2 loaded.
extern "C" int bg3le_imgui_font_info(char const* name, float* size) {
    auto* ui = bg3le::manager();
    if (ui == nullptr || name == nullptr) return 0;
    const std::lock_guard<std::mutex> held(bg3le::frame_lock());
    auto* font = ui->GetFont(bg3se::FixedString(name));
    if (font == nullptr) return 0;
    *size = font->SizePixels;
    return font->Font != nullptr ? 2 : 1;
}

extern "C" void bg3le_imgui_set_ui_scale(float scale) {
    if (auto* ui = bg3le::manager()) ui->SetUIScaleMultiplier(scale);
}

extern "C" void bg3le_imgui_set_font_scale(float scale) {
    if (auto* ui = bg3le::manager()) ui->SetFontScaleMultiplier(scale);
}

extern "C" bool bg3le_imgui_viewport_size(int* width, int* height) {
    auto* ui = bg3le::manager();
    if (ui == nullptr) return false;
    const auto size = ui->GetViewportSize();
    *width = size.x;
    *height = size.y;
    return true;
}

// Names the window, and the item within it, whose layout the next frame
// should record. Its numbers are read back with bg3le_imgui_window_geometry.
extern "C" void bg3le_imgui_watch(char const* window, char const* item) {
    const std::lock_guard<std::mutex> held(bg3le::watch_lock());
    bg3le::g_watch_name = window != nullptr ? window : "";
    bg3le::g_watch_item = item != nullptr ? item : "";
    bg3le::g_watch_valid = false;
}

extern "C" bool bg3le_imgui_window_geometry(float* out, std::size_t count) {
    const std::lock_guard<std::mutex> held(bg3le::watch_lock());
    if (!bg3le::g_watch_valid) return false;
    for (std::size_t i = 0; i < count && i < bg3le::kGeometrySlots; ++i) {
        out[i] = bg3le::g_watch[i];
    }
    return true;
}

extern "C" bool bg3le_imgui_hovered(char* name, std::size_t size,
                                    unsigned* hoveredId, unsigned* activeId,
                                    unsigned* navId, unsigned* watchedId) {
    const std::lock_guard<std::mutex> held(bg3le::watch_lock());
    if (name != nullptr && size > 0) {
        std::snprintf(name, size, "%s", bg3le::g_hovered_name);
    }
    if (hoveredId != nullptr) *hoveredId = bg3le::g_hovered_id;
    if (activeId != nullptr) *activeId = bg3le::g_active_id;
    if (navId != nullptr) *navId = bg3le::g_nav_id;
    if (watchedId != nullptr) *watchedId = bg3le::g_watch_item_id;
    return true;
}

// Holds the mouse at a position, or stops holding it.
extern "C" void bg3le_imgui_hold_mouse(float x, float y, bool holding) {
    const std::lock_guard<std::mutex> held(bg3le::input_lock());
    bg3le::g_holding = holding;
    bg3le::g_hold_x = x;
    bg3le::g_hold_y = y;
}

// A press and a release at a position, each taking its own frame.
extern "C" void bg3le_imgui_click_at(float x, float y, int button) {
    const std::lock_guard<std::mutex> held(bg3le::input_lock());
    bg3le::pending_input().push_back(bg3le::MouseInput{x, y, button, true});
    bg3le::pending_input().push_back(bg3le::MouseInput{x, y, button, false});
}

// Where imgui thinks the mouse is, how big the display is, and whether the
// left button is down.
extern "C" void bg3le_imgui_input_state(float* mouseX, float* mouseY,
                                        float* displayW, float* displayH,
                                        bool* mouseDown, bool* wantMouse,
                                        bool* hoveredWindow,
                                        bool* navNoHover) {
    *wantMouse = bg3le::g_want_mouse.load(std::memory_order_relaxed);
    *hoveredWindow = bg3le::g_hovered_window.load(std::memory_order_relaxed);
    *navNoHover = bg3le::g_nav_no_hover.load(std::memory_order_relaxed);
    *mouseX = bg3le::g_mouse_x.load(std::memory_order_relaxed);
    *mouseY = bg3le::g_mouse_y.load(std::memory_order_relaxed);
    *displayW = bg3le::g_display_w.load(std::memory_order_relaxed);
    *displayH = bg3le::g_display_h.load(std::memory_order_relaxed);
    *mouseDown = bg3le::g_mouse_down.load(std::memory_order_relaxed);
}

extern "C" void bg3le_imgui_frame_stats(std::uint64_t* frames,
                                        int* vertices, int* lists,
                                        int* drawnFrames) {
    if (drawnFrames != nullptr) {
        *drawnFrames = bg3le::g_imgui_frames.load(std::memory_order_relaxed);
    }
    if (frames != nullptr) {
        *frames = bg3le::g_frames.load(std::memory_order_relaxed);
    }
    if (vertices != nullptr) {
        *vertices = bg3le::g_last_vertices.load(std::memory_order_relaxed);
    }
    if (lists != nullptr) {
        *lists = bg3le::g_last_lists.load(std::memory_order_relaxed);
    }
}

extern "C" void bg3le_imgui_status(bool* wanted, bool* started,
                                   bool* initialized) {
    if (wanted != nullptr) *wanted = imgui_overlay_wanted();
    if (started != nullptr) *started = g_started;
    if (initialized != nullptr) *initialized = imgui_overlay_ready();
}

}  // namespace bg3le

// ShowErrorAndExitGame's message, drawn by the overlay itself so it looks
// the same on every machine -- no desktop dialog, no window of its own.
namespace {
struct ErrorDialog {
    std::mutex Lock;
    std::string Title;
    std::string Message;
    bool Showing = false;
};

ErrorDialog& error_dialog() {
    static ErrorDialog d;
    return d;
}

std::atomic<pthread_t> g_overlay_thread{};
std::atomic<bool> g_overlay_thread_known{false};
}  // namespace

// Called from inside IMGUIManager::Update's frame, after the mods' windows,
// so it is drawn over them.
extern "C" void bg3le_imgui_draw_error() {
    g_overlay_thread.store(pthread_self());
    g_overlay_thread_known.store(true);

    auto& d = error_dialog();
    std::unique_lock<std::mutex> held(d.Lock);
    if (!d.Showing) return;

    const std::string id = d.Title + "##bg3le_error";
    if (!ImGui::IsPopupOpen(id.c_str())) ImGui::OpenPopup(id.c_str());

    auto const& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x * 0.25f, 0),
                                        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.8f));
    if (!ImGui::BeginPopupModal(id.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                                    | ImGuiWindowFlags_NoScrollbar
                                    | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.45f);
    ImGui::TextUnformatted(d.Message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("The game will close.");
    ImGui::Spacing();

    const float width = ImGui::GetFontSize() * 6.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button("OK", ImVec2(width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)
        || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        std::_Exit(1);
    }
    ImGui::EndPopup();
}

// Shows the message, and ends the game when it is dismissed. Never waits
// for it: the frame waits on the game thread, so a caller blocked there
// would stop the very frame that draws the dialog. False if the overlay is
// not drawing.
extern "C" bool bg3le_imgui_show_error(char const* title, char const* message) {
    if (!bg3le::g_started || !g_overlay_thread_known.load()) return false;
    auto& d = error_dialog();
    const std::lock_guard<std::mutex> held(d.Lock);
    d.Title = title;
    d.Message = message;
    d.Showing = true;
    return true;
}

