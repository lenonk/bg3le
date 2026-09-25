// bg3se's builtin Lua bundle: the scripts builtin:// paths name, embedded at
// build time from vendor/bg3se/BG3Extender/LuaScripts (by Norbyte and the
// bg3se contributors).

#include <cstddef>
#include <cstring>

namespace {
struct BuiltinScript {
    char const* Path;
    unsigned char const* Data;
    std::size_t Size;
};
#include "builtin_lua.inc"
}  // namespace

// The script at path (relative to LuaScripts, e.g. "Tests/TestHelpers.lua"),
// or nullptr.
extern "C" char const* bg3le_builtin_lua(char const* path, std::size_t* size) {
    if (path == nullptr) return nullptr;
    for (auto const& script : kBuiltinScripts) {
        if (std::strcmp(script.Path, path) == 0) {
            *size = script.Size;
            return reinterpret_cast<char const*>(script.Data);
        }
    }
    return nullptr;
}
