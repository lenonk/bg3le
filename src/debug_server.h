// LuaDebug-protocol server, so existing clients (bg3lua) work unchanged.
#pragma once

namespace bg3le {

// Starts the listener thread. No-op if already running or BG3LE_DEBUG=0.
void debug_server_start();

// Runs queued evaluations. Must be called from the story thread; the socket
// thread never touches Lua or the engine itself.
void debug_server_pump();

// The same for client-context evaluations, from the client's own tick.
void debug_server_pump_client();

// Forwards Lua print() output to the attached client.
void debug_server_output(const char* text, int severity = 0);

// Lifecycle status: written to the log, forwarded to an attached client, and
// retained so a client that connects later still sees what happened. This is
// the stream bg3se surfaces in the debugger.
void statusf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Records the calling thread as the one safe for Lua and engine calls.
// Called from Osiris entry points, which run on the story thread.
void debug_server_note_story_thread();

// Whether the caller is that thread. False until it has been recorded.
bool debug_server_on_story_thread();

// Cheap poll from a hot interposed libc call: pumps only when work is
// pending and we are on the story thread. Designed to cost ~nothing
// otherwise, since it runs on every clock_gettime.
void debug_server_tick();

}  // namespace bg3le
