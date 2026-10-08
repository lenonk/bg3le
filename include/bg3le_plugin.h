/* bg3le native plugin API.
 *
 * A plugin is a shared library in ~/.local/share/bg3le/plugins (or
 * $BG3LE_PLUGINS_DIR). bg3le dlopen()s each one when the game's event loop
 * starts, once the engine and its allocator are up, and calls its
 * bg3le_plugin_init(). No LD_PRELOAD and no launch option is involved.
 *
 * Plugins build against this header alone: everything bg3le offers comes
 * through the host table, so a plugin links against nothing of bg3le's.
 */
#ifndef BG3LE_PLUGIN_H
#define BG3LE_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BG3LE_PLUGIN_ABI 1

/* Marks bg3le_plugin_init exported when building with -fvisibility=hidden. */
#define BG3LE_PLUGIN_EXPORT __attribute__((visibility("default")))

typedef union SDL_Event SDL_Event;

/* Opaque: identifies the calling plugin to the host. */
typedef struct bg3le_plugin bg3le_plugin;

typedef enum bg3le_setting_type {
    BG3LE_SETTING_BOOL = 0,  /* value points to an int: 0 or 1 */
    BG3LE_SETTING_INT = 1,   /* value points to an int */
    BG3LE_SETTING_FLOAT = 2, /* value points to a float */
} bg3le_setting_type;

/* An event the game is about to receive, after bg3le's overlay has had it.
 * Return nonzero to keep it from the game. Runs on the game's main thread. */
typedef int (*bg3le_event_handler)(void* user, SDL_Event* event);

/* Once per client frame, on the client's game thread (not the main thread),
 * after the engine's update and the client's Lua Tick. dt is the seconds since
 * the previous frame. */
typedef void (*bg3le_frame_handler)(void* user, double dt);

typedef struct bg3le_host {
    uint32_t abi;          /* BG3LE_PLUGIN_ABI of this bg3le */
    uint32_t size;         /* sizeof(bg3le_host): fields past it don't exist */
    const char* version;   /* bg3le's version, e.g. "v0.1.0" */

    /* Name and version shown in Ext.Plugins.List(); the file name until set. */
    void (*describe)(bg3le_plugin* self, const char* name, const char* version);

    /* A line in bg3le's log; warn also shows it on the debug console. */
    void (*log)(bg3le_plugin* self, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));
    void (*warn)(bg3le_plugin* self, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));

    /* Called for every event the game is about to receive. 0 on success. */
    int (*add_event_handler)(bg3le_plugin* self, bg3le_event_handler handler, void* user);

    /* SDL's own function by name, past bg3le's hooks, or NULL. A dlopen()ed
     * library can't reach it with dlsym(RTLD_NEXT). */
    void* (*sdl_function)(const char* name);

    /* A variable of the plugin's, readable and writable from Lua as
     * Ext.Plugins.Get/Set(<plugin name>, id). bg3le writes *value directly,
     * clamped to [min, max] for numbers when min < max. 0 on success. */
    int (*add_setting)(bg3le_plugin* self, const char* id, bg3le_setting_type type,
                       void* value, double min, double max);

    /* Called once per client frame. Only from bg3le_plugin_init; 0 on success.
     * Added after v0.3.4: check host->size covers it before calling. */
    int (*add_frame_handler)(bg3le_plugin* self, bg3le_frame_handler handler, void* user);
} bg3le_host;

/* Exported by every plugin. Return 0 on success. Anything else reports it as
 * failed in Ext.Plugins.List() and drops its handlers and settings; it stays
 * loaded, since it may already have patched code to jump into it. The host
 * table outlives the plugin, so it may keep the pointer.
 *
 * Settings persist in <plugin file name>.settings.json beside it, which bg3le
 * rewrites after a successful init and whenever Lua changes one; it can also
 * be edited by hand with the game closed. add_setting() applies the saved
 * value before it returns. */
BG3LE_PLUGIN_EXPORT int bg3le_plugin_init(const bg3le_host* host, bg3le_plugin* self);

#ifdef __cplusplus
}
#endif

#endif /* BG3LE_PLUGIN_H */
