// The exported half of bg3le's Detours stand-in, for the Vulkan entry points
// bg3se's ImGui overlay wraps.
//
// See src/detour_interpose.cpp for the other half. In short: bg3se registers
// a replacement against a target function, and on Linux the way to make the
// game's call land on that replacement is to export the symbol here and let
// the dynamic linker route it.
//
// The pairs are registered with the shim up front, and that is the part that
// took a stack overflow to get right. bg3se's EnableHooks does
//
//     auto f = vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
//     CreateInstanceHook_.Wrap(ResolveFunctionTrampoline(f));
//
// and because src/vulkan_memory.cpp diverts these names here, the pointer it
// registers against is a forwarder, not the loader's function. The first
// version handed that same pointer back as "the original to call through", so
// bg3se's post-hook called the forwarder, which found the hook, which called
// the post-hook, until the stack ran out -- 0x7f..fac in vkCreateInstance
// under two hundred frames of StaticPostHook.
//
// So every forwarder tells the shim which function it stands in for, before
// any hook is registered, and DetourAttachEx records and trampolines against
// the real one.
//
// On, as upstream's is; BG3LE_IMGUI=0 turns it off. vkQueuePresentKHR runs
// every frame and the overlay does real Vulkan work inside it.
//
// The hooked functions are bg3se's, by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); the forwarding is ours.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>

#include "detour_interpose.h"
#include "log.h"

namespace bg3le {

void imgui_overlay_start();
void vulkan_register_forwarders();

bool imgui_overlay_wanted() {
    static const bool on = [] {
        const char* opt = std::getenv("BG3LE_IMGUI");
        return opt == nullptr || opt[0] != '0';
    }();
    return on;
}

}  // namespace bg3le

namespace {

// The loader's own function, past ours. Same reasoning as
// src/vulkan_memory.cpp: RTLD_NEXT alone misses if nothing has pulled the
// loader in yet, and a null here would turn a forwarder into a crash.
void* loader() {
    static void* handle = [] () -> void* {
        void* h = ::dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (h == nullptr) h = ::dlopen("libvulkan.so.1", RTLD_LAZY);
        return h;
    }();
    return handle;
}

template <typename Fn>
Fn real(char const* name) {
    if (void* next = ::dlsym(RTLD_NEXT, name)) return (Fn)next;
    if (void* h = loader()) {
        if (void* found = ::dlsym(h, name)) return (Fn)found;
    }
    return nullptr;
}

// The replacement registered against the loader's function, or null.
//
// One key, because DetourAttachEx resolves a forwarder to the function behind
// it before recording -- see detour_register_forwarder for why that matters.
template <typename Fn>
Fn hooked(Fn original) {
    if (original == nullptr || !bg3le::imgui_overlay_wanted()) return nullptr;
    return (Fn)bg3le::detour_for((void const*)original);
}

}  // namespace

// One forwarder per wrapped entry point. Each resolves the loader's function,
// asks whether a replacement is registered against it, and calls whichever it
// should.
#define BG3LE_FORWARD(ret, name, params, args)                                \
    extern "C" ret name params {                                              \
        using Fn = ret (*) params;                                            \
        static const Fn next = real<Fn>(#name);                               \
        const Fn take = hooked<Fn>(next);                                     \
        if (take != nullptr && take != (Fn)&name) return take args;           \
        return next args;                                                     \
    }

// The one forwarder that is not written by the macro: the overlay has to be
// built here, after the engine heap exists and before the instance does, and
// the hook lookup has to happen after that or there is nothing to find.
extern "C" VkResult vkCreateInstance(VkInstanceCreateInfo const* info,
                                     VkAllocationCallbacks const* alloc,
                                     VkInstance* out) {
    using Fn = VkResult (*)(VkInstanceCreateInfo const*,
                            VkAllocationCallbacks const*, VkInstance*);
    static const Fn next = real<Fn>("vkCreateInstance");

    bg3le::vulkan_register_forwarders();
    bg3le::imgui_overlay_start();

    const Fn take = hooked<Fn>(next);
    if (take != nullptr && take != (Fn)&vkCreateInstance) {
        return take(info, alloc, out);
    }
    return next(info, alloc, out);
}

// The physical device the game renders with, for the swapchain check below.
static VkPhysicalDevice g_physical = VK_NULL_HANDLE;

extern "C" VkResult vkCreateDevice(VkPhysicalDevice phys,
                                   VkDeviceCreateInfo const* info,
                                   VkAllocationCallbacks const* alloc,
                                   VkDevice* out) {
    using Fn = VkResult (*)(VkPhysicalDevice, VkDeviceCreateInfo const*,
                            VkAllocationCallbacks const*, VkDevice*);
    static const Fn next = real<Fn>("vkCreateDevice");
    g_physical = phys;
    const Fn take = hooked<Fn>(next);
    if (take != nullptr && take != (Fn)&vkCreateDevice) return take(phys, info, alloc, out);
    return next(phys, info, alloc, out);
}

BG3LE_FORWARD(void, vkDestroyDevice,
              (VkDevice dev, VkAllocationCallbacks const* alloc),
              (dev, alloc))

BG3LE_FORWARD(VkResult, vkCreatePipelineCache,
              (VkDevice dev, VkPipelineCacheCreateInfo const* info,
               VkAllocationCallbacks const* alloc, VkPipelineCache* out),
              (dev, info, alloc, out))

// An HDR swapchain is also made copyable, which the overlay's compositor
// needs to read the game's frame (src/vendor/imgui_hdr.cpp).
extern "C" VkResult vkCreateSwapchainKHR(VkDevice dev,
                                         VkSwapchainCreateInfoKHR const* info,
                                         VkAllocationCallbacks const* alloc,
                                         VkSwapchainKHR* out) {
    using Fn = VkResult (*)(VkDevice, VkSwapchainCreateInfoKHR const*,
                            VkAllocationCallbacks const*, VkSwapchainKHR*);
    static const Fn next = real<Fn>("vkCreateSwapchainKHR");
    using Caps = VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
    static const Caps caps = real<Caps>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkSwapchainCreateInfoKHR copy = *info;
    const bool hdr = info->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
        || info->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    if (hdr && bg3le::imgui_overlay_wanted() && caps != nullptr
        && g_physical != VK_NULL_HANDLE
        && (info->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        VkSurfaceCapabilitiesKHR have{};
        if (caps(g_physical, info->surface, &have) == VK_SUCCESS
            && (have.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
            copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
    }

    const Fn take = hooked<Fn>(next);
    if (take != nullptr && take != (Fn)&vkCreateSwapchainKHR) return take(dev, &copy, alloc, out);
    return next(dev, &copy, alloc, out);
}

BG3LE_FORWARD(void, vkDestroySwapchainKHR,
              (VkDevice dev, VkSwapchainKHR chain,
               VkAllocationCallbacks const* alloc),
              (dev, chain, alloc))

BG3LE_FORWARD(VkResult, vkQueuePresentKHR,
              (VkQueue queue, VkPresentInfoKHR const* info), (queue, info))

#undef BG3LE_FORWARD

namespace {

struct Entry {
    char const* Name;
    void* Fn;
};

Entry const kEntries[] = {
    {"vkCreateInstance", (void*)&vkCreateInstance},
    {"vkCreateDevice", (void*)&vkCreateDevice},
    {"vkDestroyDevice", (void*)&vkDestroyDevice},
    {"vkCreatePipelineCache", (void*)&vkCreatePipelineCache},
    {"vkCreateSwapchainKHR", (void*)&vkCreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (void*)&vkDestroySwapchainKHR},
    {"vkQueuePresentKHR", (void*)&vkQueuePresentKHR},
};

}  // namespace

namespace bg3le {

// Every pair at once, before any hook is registered.
//
// Eagerly rather than as each forwarder is first called, because bg3se wraps
// the later entry points from inside the earlier ones' hooks: vkCreateDevice
// is wrapped during vkCreateInstance, and its forwarder has not run yet, so
// waiting for it to resolve its own function would be too late.
void vulkan_register_forwarders() {
    static bool done = false;
    if (done || !imgui_overlay_wanted()) return;
    done = true;

    for (Entry const& e : kEntries) {
        void* real_one = ::dlsym(RTLD_NEXT, e.Name);
        if (real_one == nullptr && loader() != nullptr) {
            real_one = ::dlsym(loader(), e.Name);
        }
        if (real_one == nullptr || real_one == e.Fn) continue;
        detour_register_forwarder(e.Fn, real_one);
    }
    logf("imgui: registered %zu Vulkan forwarders",
         sizeof(kEntries) / sizeof(kEntries[0]));
}

}  // namespace bg3le

// Whether a proc-address lookup should be answered with a forwarder.
//
// Called by src/vulkan_memory.cpp, which owns the two proc-address hooks.
// A C symbol, and weakly referenced there, because that file is also built
// into memsteer.so -- which has no ImGui overlay and must not drag the
// forwarders in.
extern "C" void* bg3le_vulkan_forwarder(char const* name) {
    if (name == nullptr || !bg3le::imgui_overlay_wanted()) return nullptr;

    bg3le::vulkan_register_forwarders();
    for (Entry const& e : kEntries) {
        if (std::strcmp(name, e.Name) == 0) return e.Fn;
    }
    return nullptr;
}
