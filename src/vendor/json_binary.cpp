// Ext.Json's binary form, through upstream's own reader and writer
// (Lua/Libs/JsonBinary.inl, by Norbyte and the bg3se contributors). Both
// work on a plain lua_State, so bg3le's states can use them as they are.

#include <stdafx.h>

#include <lauxlib.h>
#include <lua.h>
#include <Lua/Libs/Json.h>

#include <exception>

// Ext._Internal.JsonBinaryEncode(value) -> blob
extern "C" int bg3le_json_binary_encode(lua_State* L)
{
    luaL_checkany(L, 1);
    bg3se::lua::json::StringifyContext ctx;
    ctx.Binary = true;
    ctx.Beautify = false;
    bg3se::STDString blob;
    try {
        blob = bg3se::lua::json::Stringify(L, ctx, 1);
    } catch (std::exception const& e) {
        return luaL_error(L, "%s", e.what());
    }
    lua_pushlstring(L, blob.data(), blob.size());
    return 1;
}

// Ext._Internal.JsonBinaryDecode(blob) -> value
extern "C" int bg3le_json_binary_decode(lua_State* L)
{
    std::size_t length = 0;
    char const* blob = luaL_checklstring(L, 1, &length);
    if (!bg3se::lua::json::Parse(L, bg3se::StringView(blob, length), true)) {
        return luaL_error(L, "Unable to parse blob");
    }
    return 1;
}
