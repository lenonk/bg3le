// Ext.UI on bg3le's own Lua host.
//
// Included at the end of upstream's Lua/Libs/ClientUI/Module.inl, so
// upstream's class cache, type tables and custom-type builder are in scope.
// What upstream does through its binding framework is done here with plain
// Lua C functions: a Noesis object travels as a light userdata, and the
// prelude wraps it in a proxy with upstream's methods and properties.
//
// The value conversions are ports of upstream's StoredValueHelpers
// (NsHelpers.inl); the command and event plumbing replaces its
// DeferredUIEvents and UIEventHooks, which need upstream's Lua state. Both are
// by Norbyte and the bg3se contributors (https://github.com/Norbyte/bg3se).

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../log.h"
#include "../mem.h"

namespace Noesis::bg3le_ui {

namespace nsui = bg3se::ecl::lua::ui;
using Noesis::TypeHelpers;

// ---- objects -------------------------------------------------------------

// A light userdata is only as good as the object behind it; this catches the
// unmapped case rather than every stale one.
BaseObject* object_arg(lua_State* L, int index)
{
    if (!lua_islightuserdata(L, index)) {
        luaL_argerror(L, index, "expected a Noesis object");
        return nullptr;
    }
    auto o = static_cast<BaseObject*>(lua_touserdata(L, index));
    void* vtable = nullptr;
    if (o == nullptr || !bg3le::safe_read(o, &vtable, sizeof(vtable)) || vtable == nullptr) {
        luaL_argerror(L, index, "Noesis object is gone");
        return nullptr;
    }
    return o;
}

BaseObject* optional_object(lua_State* L, int index)
{
    return lua_isnoneornil(L, index) ? nullptr : object_arg(L, index);
}

void push_object(lua_State* L, void const* o)
{
    if (o == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlightuserdata(L, const_cast<void*>(o));
    }
}

bool is_a(BaseObject const* o, TypeClass const* cls)
{
    return o != nullptr && cls != nullptr
        && TypeHelpers::IsDescendantOf(o->GetClassType(), cls);
}

template <class T>
T* require(lua_State* L, int index, TypeClass const* cls, char const* what)
{
    auto o = object_arg(L, index);
    if (!is_a(o, cls)) {
        luaL_error(L, "%s is not a %s", o->GetClassType()->GetName(), what);
        return nullptr;
    }
    return static_cast<T*>(o);
}

void ensure_symbols()
{
    gStaticSymbols.Initialize();
}

// ---- values: ports of StoredValueHelpers ---------------------------------

void push_floats(lua_State* L, float const* v, int n)
{
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_pushnumber(L, v[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

bool read_floats(lua_State* L, int index, float* out, int n)
{
    if (!lua_istable(L, index)) return false;
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, index, i + 1);
        out[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

void push_enum(lua_State* L, TypeEnum const* type, uint64_t val)
{
    for (auto const& e : type->mValues) {
        if (e.second == val) {
            lua_pushstring(L, e.first.Str());
            return;
        }
    }
    lua_pushinteger(L, (lua_Integer)val);
}

template <class T>
T raw(void* const& val)
{
    return *reinterpret_cast<T const*>(&val);
}

void push_value(lua_State* L, Type const* type, void* val, Type const* objectType, Symbol const* name);

void push_boxed_or_object(lua_State* L, Type const* type, void* val, Type const* objectType, Symbol const* name)
{
    auto& classes = gStaticSymbols.TypeClasses;
    auto obj = static_cast<BaseObject*>(val);
    if (obj == nullptr) {
        lua_pushnil(L);
        return;
    }

    // Upstream hands out no command of these data contexts.
    if (objectType != nullptr && name != nullptr
        && obj->GetClassType()->GetBase() == classes.BaseCommand.Type) {
        auto& names = gStaticSymbols.DeferredNames;
        if (objectType->GetTypeId() == names.DCSavegames) {
            lua_pushnil(L);
            return;
        }
        if (objectType->GetTypeId() == names.DCModBrowser
            && (*name == names.EndorseModCommand || *name == names.ReportModCommand
                || *name == names.SubscribeModCommand || *name == names.UninstallModCommand
                || *name == names.UnsubscribeModCommand)) {
            lua_pushnil(L);
            return;
        }
    }

    if (obj->GetClassType()->GetBase() == classes.BoxedValue.Type) {
        auto boxed = static_cast<BoxedValue*>(obj);
        push_value(L, boxed->GetValueType(), const_cast<void*>(boxed->GetValuePtr()), objectType, name);
    } else {
        push_object(L, obj);
    }
}

void push_value(lua_State* L, Type const* type, void* val, Type const* objectType, Symbol const* name)
{
    auto& classes = gStaticSymbols.TypeClasses;
    auto& types = gStaticSymbols.Types;
    auto typeOfType = type->GetClassType();

    if (typeOfType == types.TypeConst.Type) {
        push_value(L, static_cast<TypeConst const*>(type)->GetContentType(), val, objectType, name);
    } else if (type == types.Int8.Type) {
        lua_pushinteger(L, raw<int8_t>(val));
    } else if (type == types.Int16.Type) {
        lua_pushinteger(L, raw<int16_t>(val));
    } else if (type == types.Int32.Type) {
        lua_pushinteger(L, raw<int32_t>(val));
    } else if (type == types.Int64.Type) {
        lua_pushinteger(L, raw<int64_t>(val));
    } else if (type == types.UInt8.Type) {
        lua_pushinteger(L, raw<uint8_t>(val));
    } else if (type == types.UInt16.Type) {
        lua_pushinteger(L, raw<uint16_t>(val));
    } else if (type == types.UInt32.Type) {
        lua_pushinteger(L, raw<uint32_t>(val));
    } else if (type == types.UInt64.Type) {
        lua_pushinteger(L, (lua_Integer)raw<uint64_t>(val));
    } else if (type == types.Single.Type) {
        lua_pushnumber(L, raw<float>(val));
    } else if (type == types.Double.Type) {
        lua_pushnumber(L, raw<double>(val));
    } else if (type == types.Bool.Type) {
        lua_pushboolean(L, raw<bool>(val) ? 1 : 0);
    } else if (type == types.Symbol.Type) {
        lua_pushstring(L, raw<Symbol>(val).Str());
    } else if (type == types.Color.Type) {
        push_floats(L, static_cast<float const*>(val), 4);
    } else if (type == types.Vector2.Type || type == types.Point.Type) {
        push_floats(L, reinterpret_cast<float const*>(&val), 2);
    } else if (type == types.Vector3.Type) {
        push_floats(L, static_cast<float const*>(val), 3);
    } else if (type == types.Rect.Type || type == types.Thickness.Type
               || type == types.CornerRadius.Type) {
        push_floats(L, static_cast<float const*>(val), 4);
    } else if (type == types.Int32Rect.Type) {
        auto v = static_cast<int32_t const*>(val);
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; ++i) {
            lua_pushinteger(L, v[i]);
            lua_rawseti(L, -2, i + 1);
        }
    } else if (type == types.GridLength.Type) {
        auto g = raw<GridLengthHelper>(val);
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)g.GridUnitType);
        lua_setfield(L, -2, "GridUnitType");
        lua_pushnumber(L, g.Value);
        lua_setfield(L, -2, "Value");
    } else if (type == types.String.Type) {
        lua_pushstring(L, static_cast<String const*>(val)->Str());
    } else if (type == types.ICommand.Type) {
        push_object(L, val != nullptr ? static_cast<ICommand*>(val)->GetBaseObject() : nullptr);
    } else if (type == types.IValueConverter.Type || type == types.IMultiValueConverter.Type) {
        lua_pushnil(L);  // upstream: unsupported
    } else if (type == types.Uri.Type) {
        lua_pushstring(L, static_cast<Uri const*>(val)->mUri.Str());
    } else if (TypeHelpers::IsDescendantOf(type, classes.BaseObject.Type)) {
        push_boxed_or_object(L, type, val, objectType, name);
    } else if (TypeHelpers::IsDescendantOf(typeOfType, classes.TypeEnum.Type)) {
        push_enum(L, static_cast<TypeEnum const*>(type), (uint64_t)raw<int>(val));
    } else {
        ERR("Don't know how to fetch properties of type '%s'", type->GetName());
        lua_pushnil(L);
    }
}

void push_stored(lua_State* L, Type const* type, StoredValue const* o, Type const* objectType, Symbol const* name)
{
    if (!o->flags.isInitialized) {
        lua_pushnil(L);
    } else if (o->flags.isComplex) {
        if (o->flags.isExpression) {
            lua_pushnil(L);
        } else {
            push_value(L, type, o->value.complex->base, objectType, name);
        }
    } else {
        push_value(L, type, o->value.simple, objectType, name);
    }
}

template <class T>
T copy_of(TypeProperty const* prop, BaseObject const* obj)
{
    T value{};
    prop->GetCopy(obj, &value);
    return value;
}

void push_property(lua_State* L, BaseObject const* obj, TypeProperty const* prop)
{
    auto& types = gStaticSymbols.Types;
    auto& classes = gStaticSymbols.TypeClasses;
    auto type = prop->GetContentType();
    auto typeOfType = type->GetClassType();
    auto name = prop->GetName();
    auto objType = obj->GetClassType();
    auto ref = [&]() { return const_cast<void*>(prop->Get(obj)); };

    if (type == types.CStringPtr.Type) {
        lua_pushstring(L, copy_of<char const*>(prop, obj));
    } else if (type == types.CTypePtr.Type) {
        push_object(L, copy_of<Type*>(prop, obj));
    } else if (typeOfType == types.TypePtr.Type) {
        auto value = static_cast<Ptr<BaseRefCounted>*>(ref());
        push_value(L, static_cast<TypePtr const*>(type)->GetStaticContentType(), value->GetPtr(), objType, &name);
    } else if (typeOfType == types.TypePointer.Type) {
        push_value(L, static_cast<TypePointer const*>(type)->GetStaticContentType(),
                   copy_of<BaseObject*>(prop, obj), objType, &name);
    } else if (TypeHelpers::IsDescendantOf(typeOfType, classes.TypeEnum.Type)) {
        push_enum(L, static_cast<TypeEnum const*>(type), (uint64_t)copy_of<int64_t>(prop, obj));
    } else if (type == types.Int8.Type) {
        lua_pushinteger(L, copy_of<int8_t>(prop, obj));
    } else if (type == types.Int16.Type) {
        lua_pushinteger(L, copy_of<int16_t>(prop, obj));
    } else if (type == types.Int32.Type) {
        lua_pushinteger(L, copy_of<int32_t>(prop, obj));
    } else if (type == types.Int64.Type) {
        lua_pushinteger(L, copy_of<int64_t>(prop, obj));
    } else if (type == types.UInt8.Type) {
        lua_pushinteger(L, copy_of<uint8_t>(prop, obj));
    } else if (type == types.UInt16.Type) {
        lua_pushinteger(L, copy_of<uint16_t>(prop, obj));
    } else if (type == types.UInt32.Type) {
        lua_pushinteger(L, copy_of<uint32_t>(prop, obj));
    } else if (type == types.UInt64.Type) {
        lua_pushinteger(L, (lua_Integer)copy_of<uint64_t>(prop, obj));
    } else if (type == types.Single.Type) {
        lua_pushnumber(L, copy_of<float>(prop, obj));
    } else if (type == types.Double.Type) {
        lua_pushnumber(L, copy_of<double>(prop, obj));
    } else if (type == types.Bool.Type) {
        lua_pushboolean(L, copy_of<bool>(prop, obj) ? 1 : 0);
    } else if (type == types.Symbol.Type) {
        lua_pushstring(L, copy_of<Symbol>(prop, obj).Str());
    } else if (type == types.Color.Type || type == types.Rect.Type
               || type == types.Thickness.Type || type == types.CornerRadius.Type) {
        push_floats(L, static_cast<float const*>(ref()), 4);
    } else if (type == types.Vector2.Type || type == types.Point.Type) {
        push_floats(L, static_cast<float const*>(ref()), 2);
    } else if (type == types.Vector3.Type) {
        push_floats(L, static_cast<float const*>(ref()), 3);
    } else if (type == types.ICommand.Type) {
        auto cmd = *static_cast<ICommand**>(ref());
        push_object(L, cmd != nullptr ? cmd->GetBaseObject() : nullptr);
    } else if (type == types.IValueConverter.Type || type == types.IMultiValueConverter.Type) {
        lua_pushnil(L);
    } else if (type == types.String.Type) {
        lua_pushstring(L, static_cast<String const*>(ref())->Str());
    } else if (type == types.Uri.Type) {
        lua_pushstring(L, static_cast<Uri const*>(ref())->mUri.Str());
    } else if (type == types.LocaString.Type) {
        lua_pushstring(L, static_cast<bg3se::TranslatedString const*>(ref())->Handle.Handle.GetString());
    } else {
        ERR("Don't know how to fetch property %s:%s of type '%s'", objType->GetName(),
            name.Str(), type->GetName());
        lua_pushnil(L);
    }
}

void push_dependency(lua_State* L, DependencyObject const* o, DependencyProperty const* prop)
{
    auto it = o->mValues.Find(prop);
    if (it == o->mValues.End()) {
        lua_pushnil(L);
        return;
    }
    auto name = prop->GetName();
    push_stored(L, prop->GetType(), it->value, o->GetClassType(), &name);
}

template <class T>
StoredValueHolder hold(T const& v)
{
    return StoredValueHolder(v, StoredValueCopy{});
}

std::optional<StoredValueHolder> floats_value(lua_State* L, int index, int n)
{
    float v[4] = {0, 0, 0, 0};
    if (!read_floats(L, index, v, n)) {
        luaL_error(L, "expected a table of %d numbers", n);
        return {};
    }
    if (n == 2) return hold(glm::vec2(v[0], v[1]));
    if (n == 3) return hold(glm::vec3(v[0], v[1], v[2]));
    return hold(glm::vec4(v[0], v[1], v[2], v[3]));
}

std::optional<StoredValueHolder> get_value(lua_State* L, Type const* type, int index)
{
    auto& classes = gStaticSymbols.TypeClasses;
    auto& types = gStaticSymbols.Types;

    if (type == types.Int8.Type) return hold((int8_t)luaL_checkinteger(L, index));
    if (type == types.Int16.Type) return hold((int16_t)luaL_checkinteger(L, index));
    if (type == types.Int32.Type) return hold((int32_t)luaL_checkinteger(L, index));
    if (type == types.Int64.Type) return hold((int64_t)luaL_checkinteger(L, index));
    if (type == types.UInt8.Type) return hold((uint8_t)luaL_checkinteger(L, index));
    if (type == types.UInt16.Type) return hold((uint16_t)luaL_checkinteger(L, index));
    if (type == types.UInt32.Type) return hold((uint32_t)luaL_checkinteger(L, index));
    if (type == types.UInt64.Type) return hold((uint64_t)luaL_checkinteger(L, index));
    if (type == types.Single.Type) return hold((float)luaL_checknumber(L, index));
    if (type == types.Double.Type) return hold((double)luaL_checknumber(L, index));
    if (type == types.Bool.Type) return hold(lua_toboolean(L, index) != 0);
    if (type == types.Symbol.Type) return hold(Symbol(luaL_checkstring(L, index)));
    if (type == types.Color.Type || type == types.Rect.Type || type == types.Thickness.Type
        || type == types.CornerRadius.Type) {
        return floats_value(L, index, 4);
    }
    if (type == types.Vector2.Type || type == types.Point.Type) return floats_value(L, index, 2);
    if (type == types.Vector3.Type) return floats_value(L, index, 3);
    if (type == types.String.Type) return hold(String(luaL_checkstring(L, index)));
    if (type == types.ICommand.Type) {
        auto obj = optional_object(L, index);
        auto cmd = obj != nullptr ? DynamicCast<ICommand*>(static_cast<BaseComponent*>(obj)) : nullptr;
        if (obj != nullptr && cmd == nullptr) {
            luaL_error(L, "%s is not a command", obj->GetClassType()->GetName());
            return {};
        }
        return hold(cmd);
    }
    if (type == types.IValueConverter.Type || type == types.IMultiValueConverter.Type) {
        return {};
    }
    if (TypeHelpers::IsDescendantOf(type->GetClassType(), classes.TypeEnum.Type)) {
        auto label = luaL_checkstring(L, index);
        auto value = TypeHelpers::StringToEnum(static_cast<TypeEnum const*>(type), label);
        if (!value) {
            luaL_error(L, "Invalid enum value '%s' for enumeration '%s'", label, type->GetName());
            return {};
        }
        return hold(*value);
    }
    if (TypeHelpers::IsDescendantOf(type, classes.BaseObject.Type)) {
        return StoredValueHolder(optional_object(L, index));
    }

    auto typeOfType = type->GetClassType();
    if (typeOfType == types.TypePtr.Type) {
        auto content = static_cast<TypePtr const*>(type)->GetStaticContentType();
        auto obj = static_cast<BaseComponent*>(object_arg(L, index));
        if (!TypeHelpers::IsDescendantOf(obj->GetClassType(), static_cast<TypeClass const*>(content))) {
            luaL_error(L, "Expected object of type '%s', got '%s'", type->GetName(), obj->GetClassType()->GetName());
            return {};
        }
        // Upstream: simulates returning a Ptr<BaseComponent>.
        obj->AddReference();
        return StoredValueHolder(static_cast<BaseObject*>(obj));
    }
    if (typeOfType == types.TypePointer.Type) {
        auto content = static_cast<TypePointer const*>(type)->GetStaticContentType();
        if (content->GetClassType() != classes.TypeClass.Type) {
            luaL_error(L, "Pointer to non-object '%s' not supported", type->GetName());
            return {};
        }
        auto obj = object_arg(L, index);
        if (!TypeHelpers::IsDescendantOf(obj->GetClassType(), static_cast<TypeClass const*>(content))) {
            luaL_error(L, "Expected object of type '%s', got '%s'", type->GetName(), obj->GetClassType()->GetName());
            return {};
        }
        return StoredValueHolder(obj);
    }

    luaL_error(L, "Don't know how to parse type '%s'", type->GetName());
    return {};
}

// ---- commands and events -------------------------------------------------

std::mutex g_lock;

// LuaDelegateCommand -> the prelude's handler id.
std::unordered_map<void const*, uint32_t> g_handlers;

struct Fired
{
    uint32_t Id;
    Ptr<BaseComponent> First;
    Ptr<BaseComponent> Second;
    std::string Event;
};

std::deque<Fired> g_commands;
std::deque<Fired> g_events;
// Keeps the last delivered one alive until Lua has had it.
Fired g_delivered;

struct Subscription
{
    uint32_t Id{ 0 };
    UIElement* Target{ nullptr };
    RoutedEvent const* Event{ nullptr };
    std::string Name;
    bool Active{ false };
};

// Index + 1 is the subscription index, and doubles as the delegate's "this",
// as upstream's DummyDelegate does.
std::vector<Subscription> g_subscriptions;

struct Relay
{
    void Handler(BaseComponent* sender, const RoutedEventArgs& args)
    {
        const auto index = (std::size_t)(uintptr_t)this;
        std::lock_guard<std::mutex> held(g_lock);
        if (index == 0 || index > g_subscriptions.size()) return;
        auto const& sub = g_subscriptions[index - 1];
        if (!sub.Active) return;
        g_events.push_back(Fired{ sub.Id, Ptr<BaseComponent>(sender), Ptr<BaseComponent>(args.source), sub.Name });
    }
};

RoutedEventHandler relay_for(std::size_t index)
{
    return RoutedEventHandler(reinterpret_cast<Relay*>((uintptr_t)index), &Relay::Handler);
}

bool take(lua_State* L, std::deque<Fired>& queue)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (queue.empty()) return false;
    g_delivered = std::move(queue.front());
    queue.pop_front();
    return true;
}

// ---- Lua functions -------------------------------------------------------

int l_root(lua_State* L)
{
    push_object(L, nsui::GetRoot());
    return 1;
}

int l_type_name(lua_State* L)
{
    auto o = object_arg(L, 1);
    lua_pushstring(L, o->GetClassType()->GetName());
    return 1;
}

int l_to_string(lua_State* L)
{
    auto o = object_arg(L, 1);
    lua_pushstring(L, o->ToString().Str());
    return 1;
}

int l_is_a(lua_State* L)
{
    ensure_symbols();
    auto o = object_arg(L, 1);
    auto type = Reflection::GetType(Symbol(luaL_checkstring(L, 2)));
    bool yes = type != nullptr && type->GetClassType() == gStaticSymbols.TypeClasses.TypeClass.Type
        && is_a(o, static_cast<TypeClass const*>(type));
    lua_pushboolean(L, yes ? 1 : 0);
    return 1;
}

int l_visual_count(lua_State* L)
{
    ensure_symbols();
    auto o = require<Visual>(L, 1, gStaticSymbols.TypeClasses.Visual.Type, "Visual");
    lua_pushinteger(L, o->GetVisualChildrenCount());
    return 1;
}

int l_visual_child(lua_State* L)
{
    ensure_symbols();
    auto o = require<Visual>(L, 1, gStaticSymbols.TypeClasses.Visual.Type, "Visual");
    auto index = (uint32_t)luaL_checkinteger(L, 2);
    push_object(L, index > 0 && index <= o->GetVisualChildrenCount() ? o->GetVisualChild(index - 1) : nullptr);
    return 1;
}

int l_visual_parent(lua_State* L)
{
    ensure_symbols();
    auto o = require<Visual>(L, 1, gStaticSymbols.TypeClasses.Visual.Type, "Visual");
    push_object(L, o->mVisualParent);
    return 1;
}

int l_child_count(lua_State* L)
{
    ensure_symbols();
    auto o = require<FrameworkElement>(L, 1, gStaticSymbols.TypeClasses.FrameworkElement.Type, "FrameworkElement");
    lua_pushinteger(L, o->GetLogicalChildrenCount());
    return 1;
}

int l_child(lua_State* L)
{
    ensure_symbols();
    auto o = require<FrameworkElement>(L, 1, gStaticSymbols.TypeClasses.FrameworkElement.Type, "FrameworkElement");
    auto index = (uint32_t)luaL_checkinteger(L, 2);
    if (index > 0 && index <= o->GetLogicalChildrenCount()) {
        push_object(L, o->GetLogicalChild(index - 1).GetPtr());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int l_parent(lua_State* L)
{
    ensure_symbols();
    auto o = require<FrameworkElement>(L, 1, gStaticSymbols.TypeClasses.FrameworkElement.Type, "FrameworkElement");
    push_object(L, o->mParent);
    return 1;
}

int l_find(lua_State* L)
{
    ensure_symbols();
    auto o = require<FrameworkElement>(L, 1, gStaticSymbols.TypeClasses.FrameworkElement.Type, "FrameworkElement");
    push_object(L, o->FindNodeName(luaL_checkstring(L, 2)));
    return 1;
}

int l_resource(lua_State* L)
{
    ensure_symbols();
    auto o = require<FrameworkElement>(L, 1, gStaticSymbols.TypeClasses.FrameworkElement.Type, "FrameworkElement");
    push_object(L, o->FindNodeResource(luaL_checkstring(L, 2), lua_toboolean(L, 3) != 0));
    return 1;
}

// UiGet(o, name) -> found, value
int l_get(lua_State* L)
{
    ensure_symbols();
    auto o = object_arg(L, 1);
    auto& cls = gClassCache.GetClass(o->GetClassType());
    auto prop = cls.Names.try_get(bg3se::FixedString(luaL_checkstring(L, 2)));
    if (prop != nullptr && prop->Property != nullptr) {
        lua_pushboolean(L, 1);
        push_property(L, o, prop->Property);
        return 2;
    }
    if (prop != nullptr && prop->DepProperty != nullptr) {
        lua_pushboolean(L, 1);
        push_dependency(L, static_cast<DependencyObject const*>(o), prop->DepProperty);
        return 2;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// UiSet(o, name, value) -> found
int l_set(lua_State* L)
{
    ensure_symbols();
    auto o = object_arg(L, 1);
    auto& cls = gClassCache.GetClass(o->GetClassType());
    auto prop = cls.Names.try_get(bg3se::FixedString(luaL_checkstring(L, 2)));
    if (prop != nullptr && prop->Property != nullptr) {
        if (prop->Property->IsReadOnly()) {
            return luaL_error(L, "Property %s of %s is read-only", prop->Property->GetName().Str(),
                              o->GetClassType()->GetName());
        }
        auto val = get_value(L, prop->Property->GetContentType(), 3);
        if (val) prop->Property->Set(o, val->IsIntegral ? &val->Value : val->Value);
        lua_pushboolean(L, 1);
        return 1;
    }
    if (prop != nullptr && prop->DepProperty != nullptr) {
        auto dp = prop->DepProperty;
        auto val = get_value(L, dp->GetType(), 3);
        if (val) {
            dp->GetValueManager()->SetValue(static_cast<DependencyObject*>(o), dp,
                val->IsIntegral ? &val->Value : val->Value, 0, nullptr, nullptr,
                Value::Destination_BaseValue);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// UiProperties(o, which): which is "all", "direct" or "dependency".
int l_properties(lua_State* L)
{
    ensure_symbols();
    auto o = object_arg(L, 1);
    std::string which = luaL_optstring(L, 2, "all");
    auto type = o->GetClassType();
    lua_newtable(L);
    if (which != "direct" && TypeHelpers::IsDescendantOf(type, gStaticSymbols.TypeClasses.DependencyObject.Type)) {
        auto dep = static_cast<DependencyObject const*>(o);
        for (auto& entry : dep->mValues) {
            auto name = entry.key->GetName();
            lua_pushstring(L, name.Str());
            push_stored(L, entry.key->GetType(), entry.value, type, &name);
            lua_settable(L, -3);
        }
    }
    if (which != "dependency") {
        auto& cls = gClassCache.GetClass(type);
        for (auto entry : cls.Names) {
            if (entry.Value().Property != nullptr) {
                lua_pushstring(L, entry.Key().GetString());
                push_property(L, o, entry.Value().Property);
                lua_settable(L, -3);
            }
        }
    }
    return 1;
}

// UiRegisterType(name, {Prop = {Type=, Notify=}}, wrappedContextType)
int l_register_type(lua_State* L)
{
    ensure_symbols();
    std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::optional<std::string> wrapped;
    if (!lua_isnoneornil(L, 3)) wrapped = luaL_checkstring(L, 3);
    std::optional<std::string_view> wrappedView;
    if (wrapped) wrappedView = *wrapped;

    bg3se::HashMap<bg3se::FixedString, bg3se::ui::CustomPropertyDefn> properties;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        auto key = lua_tostring(L, -2);
        if (key == nullptr || !lua_istable(L, -1)) {
            return luaL_error(L, "RegisterType: each property is name = {Type = ...}");
        }
        auto defn = properties.get_or_add(bg3se::FixedString(key));
        defn->Name = bg3se::FixedString(key);
        lua_getfield(L, -1, "Type");
        auto type = lua_tostring(L, -1);
        defn->Type = bg3se::FixedString(type != nullptr ? type : "");
        lua_pop(L, 1);
        lua_getfield(L, -1, "Notify");
        if (!lua_isnil(L, -1)) defn->Notify = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "WriteCallback");
        if (!lua_isnil(L, -1)) {
            bg3le::logf("Ext.UI.RegisterType(%s): WriteCallback is not supported yet", name.c_str());
        }
        lua_pop(L, 2);
    }

    auto clsName = nsui::ClassDefinitionBuilder::MakeFullName(name);
    if (Reflection::GetType(clsName) != nullptr) {
        auto dynClass = nsui::gDynamicClasses.try_get(bg3se::FixedString(clsName.Str()));
        if (!dynClass) {
            return luaL_error(L, "A Noesis type already exists with this name: %s", name.c_str());
        }
        if ((*dynClass)->MatchesDefinition(properties, wrappedView)) {
            (*dynClass)->UpdateHandlers(properties);
            lua_pushboolean(L, 1);
            return 1;
        }
        return luaL_error(L, "Attempted to re-register Noesis type '%s' with different definition", clsName.Str());
    }

    lua_pushboolean(L, nsui::ClassDefinitionBuilder::RegisterNew(L, clsName, properties, wrappedView) ? 1 : 0);
    return 1;
}

int l_instantiate(lua_State* L)
{
    ensure_symbols();
    std::string name = luaL_checkstring(L, 1);
    if (name.substr(0, 4) != "se::") name = "se::" + name;
    auto wrapped = static_cast<BaseComponent*>(optional_object(L, 2));

    auto cls = nsui::gDynamicClasses.try_get(bg3se::FixedString(name.c_str()));
    if (!cls) return luaL_error(L, "No custom class found with name '%s'", name.c_str());
    auto inst = (*cls)->Construct(wrapped);
    if (!inst) return luaL_error(L, "Unable to construct data context '%s' - invalid parameters", name.c_str());
    push_object(L, static_cast<BaseComponent*>(inst));
    return 1;
}

// UiSetHandler(command, id): 0 unbinds.
int l_set_handler(lua_State* L)
{
    ensure_symbols();
    auto o = object_arg(L, 1);
    if (o->GetClassType() != TypeOf<LuaDelegateCommand>()) {
        return luaL_error(L, "%s is not a Lua command", o->GetClassType()->GetName());
    }
    auto id = (uint32_t)luaL_checkinteger(L, 2);
    {
        std::lock_guard<std::mutex> held(g_lock);
        if (id == 0) {
            g_handlers.erase(o);
        } else {
            g_handlers[o] = id;
        }
    }
    auto cmd = static_cast<LuaDelegateCommand*>(o);
    cmd->CanExecuteChanged().Invoke(cmd, EventArgs{});
    return 0;
}

int l_can_execute(lua_State* L)
{
    ensure_symbols();
    auto o = static_cast<BaseComponent*>(object_arg(L, 1));
    auto cmd = DynamicCast<ICommand*>(o);
    if (cmd == nullptr) return luaL_error(L, "%s is not a command", o->GetClassType()->GetName());
    lua_pushboolean(L, cmd->CanExecute(static_cast<BaseComponent*>(optional_object(L, 2))) ? 1 : 0);
    return 1;
}

int l_execute(lua_State* L)
{
    ensure_symbols();
    auto o = static_cast<BaseComponent*>(object_arg(L, 1));
    auto cmd = DynamicCast<ICommand*>(o);
    if (cmd == nullptr) return luaL_error(L, "%s is not a command", o->GetClassType()->GetName());
    cmd->Execute(static_cast<BaseComponent*>(optional_object(L, 2)));
    return 0;
}

// UiTakeCommand() -> id, command, parameter
int l_take_command(lua_State* L)
{
    if (!take(L, g_commands)) return 0;
    lua_pushinteger(L, g_delivered.Id);
    push_object(L, g_delivered.First.GetPtr());
    push_object(L, g_delivered.Second.GetPtr());
    return 3;
}

// UiSubscribe(element, event, id) -> subscription index, or nothing
int l_subscribe(lua_State* L)
{
    ensure_symbols();
    auto target = require<UIElement>(L, 1, TypeOf<UIElement>(), "UIElement");
    std::string name = luaL_checkstring(L, 2);
    auto id = (uint32_t)luaL_checkinteger(L, 3);
    auto event = TypeHelpers::GetRoutedEvent(target->GetClassType(), bg3se::FixedString(name.c_str()));
    if (event == nullptr) {
        ERR("UI element %s has no event named '%s'", target->GetClassType()->GetName(), name.c_str());
        return 0;
    }

    std::size_t index;
    {
        std::lock_guard<std::mutex> held(g_lock);
        g_subscriptions.push_back(Subscription{ id, target, event, name, true });
        index = g_subscriptions.size();
    }
    target->AddHandler(event, relay_for(index));
    lua_pushinteger(L, (lua_Integer)index);
    return 1;
}

int l_unsubscribe(lua_State* L)
{
    auto index = (std::size_t)luaL_checkinteger(L, 1);
    Subscription sub;
    {
        std::lock_guard<std::mutex> held(g_lock);
        if (index == 0 || index > g_subscriptions.size() || !g_subscriptions[index - 1].Active) {
            lua_pushboolean(L, 0);
            return 1;
        }
        g_subscriptions[index - 1].Active = false;
        sub = g_subscriptions[index - 1];
    }
    void* vtable = nullptr;
    if (bg3le::safe_read(sub.Target, &vtable, sizeof(vtable)) && vtable != nullptr) {
        sub.Target->RemoveHandler(sub.Event, relay_for(index));
    }
    lua_pushboolean(L, 1);
    return 1;
}

// UiTakeEvent() -> id, sender, event name, source
int l_take_event(lua_State* L)
{
    if (!take(L, g_events)) return 0;
    lua_pushinteger(L, g_delivered.Id);
    push_object(L, g_delivered.First.GetPtr());
    lua_pushstring(L, g_delivered.Event.c_str());
    push_object(L, g_delivered.Second.GetPtr());
    return 4;
}

}  // namespace Noesis::bg3le_ui

bool bg3le_ui_command_bound(void const* command)
{
    std::lock_guard<std::mutex> held(Noesis::bg3le_ui::g_lock);
    return Noesis::bg3le_ui::g_handlers.count(command) != 0;
}

void bg3le_ui_command_fired(void* command, void* parameter)
{
    std::lock_guard<std::mutex> held(Noesis::bg3le_ui::g_lock);
    auto it = Noesis::bg3le_ui::g_handlers.find(command);
    if (it == Noesis::bg3le_ui::g_handlers.end()) return;
    Noesis::bg3le_ui::g_commands.push_back(Noesis::bg3le_ui::Fired{
        it->second,
        Noesis::Ptr<Noesis::BaseComponent>(static_cast<Noesis::BaseComponent*>(command)),
        Noesis::Ptr<Noesis::BaseComponent>(static_cast<Noesis::BaseComponent*>(parameter)),
        {} });
}

// Forgets what the old Lua states bound: their handler ids mean nothing to
// the new ones. Subscriptions stay on their elements but no longer deliver.
extern "C" void bg3le_ui_reset()
{
    std::lock_guard<std::mutex> held(Noesis::bg3le_ui::g_lock);
    Noesis::bg3le_ui::g_handlers.clear();
    Noesis::bg3le_ui::g_commands.clear();
    Noesis::bg3le_ui::g_events.clear();
    for (auto& sub : Noesis::bg3le_ui::g_subscriptions) sub.Active = false;
}

// Adds Ext.UI's C side to the table on top of the stack.
extern "C" void bg3le_ui_register(lua_State* L)
{
    static const luaL_Reg functions[] = {
        {"UiRoot", Noesis::bg3le_ui::l_root},
        {"UiTypeName", Noesis::bg3le_ui::l_type_name},
        {"UiToString", Noesis::bg3le_ui::l_to_string},
        {"UiIsA", Noesis::bg3le_ui::l_is_a},
        {"UiVisualCount", Noesis::bg3le_ui::l_visual_count},
        {"UiVisualChild", Noesis::bg3le_ui::l_visual_child},
        {"UiVisualParent", Noesis::bg3le_ui::l_visual_parent},
        {"UiChildCount", Noesis::bg3le_ui::l_child_count},
        {"UiChild", Noesis::bg3le_ui::l_child},
        {"UiParent", Noesis::bg3le_ui::l_parent},
        {"UiFind", Noesis::bg3le_ui::l_find},
        {"UiResource", Noesis::bg3le_ui::l_resource},
        {"UiGet", Noesis::bg3le_ui::l_get},
        {"UiSet", Noesis::bg3le_ui::l_set},
        {"UiProperties", Noesis::bg3le_ui::l_properties},
        {"UiRegisterType", Noesis::bg3le_ui::l_register_type},
        {"UiInstantiate", Noesis::bg3le_ui::l_instantiate},
        {"UiSetHandler", Noesis::bg3le_ui::l_set_handler},
        {"UiCanExecute", Noesis::bg3le_ui::l_can_execute},
        {"UiExecute", Noesis::bg3le_ui::l_execute},
        {"UiTakeCommand", Noesis::bg3le_ui::l_take_command},
        {"UiSubscribe", Noesis::bg3le_ui::l_subscribe},
        {"UiUnsubscribe", Noesis::bg3le_ui::l_unsubscribe},
        {"UiTakeEvent", Noesis::bg3le_ui::l_take_event},
        {nullptr, nullptr}};
    luaL_setfuncs(L, functions, 0);
}
