// Ext.Types.GetTypeInfo's registry: upstream's own TypeInformationRepository,
// built the way ScriptExtender::PostStartup builds it, on first use.
// The repository and the registrations are bg3se's (by Norbyte and the bg3se
// contributors) -- thank you.

#include <stdafx.h>

#include <GameDefinitions/Base/BaseTypeInformation.h>
#include <GameDefinitions/EnumRepository.h>
#include <GameDefinitions/Enumerations.h>

#include <cxxabi.h>

#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../log.h"

namespace bg3se::lua {
void InitObjectProxyPropertyMaps();
void RegisterLibraries();
}

extern "C" bool bg3le_game_allocator_ready();

namespace {

bool build_registry() {
    try {
        // LibraryManager::PostStartupFindLibraries does this upstream.
        if (bg3se::BitfieldRegistry::Get().BitfieldsById.empty()) bg3se::InitializeEnumerations();
        bg3se::lua::InitObjectProxyPropertyMaps();
        auto& repo = bg3se::TypeInformationRepository::GetInstance();
        repo.Initialize();
        // MSVC __int64 is int64_t; here it is long long, a type of its own.
        bg3se::GetStaticTypeInfo(bg3se::Overload<long long>{}).Type =
            const_cast<bg3se::TypeInformation*>(repo.TryGetType(bg3se::FixedString("int64")));
        bg3se::GetStaticTypeInfo(bg3se::Overload<unsigned long long>{}).Type =
            const_cast<bg3se::TypeInformation*>(repo.TryGetType(bg3se::FixedString("uint64")));
        bg3se::lua::RegisterLibraries();
        repo.Finalize();
        return true;
    } catch (std::exception const& e) {
        bg3le::logf("types: building the type registry failed: %s", e.what());
        return false;
    }
}

}  // namespace

// The registry's TypeInformation for a type name, or null.
extern "C" void const* bg3le_type_info(char const* name) {
    static std::once_flag once;
    static bool built = false;
    if (name == nullptr || !bg3le_game_allocator_ready()) return nullptr;
    std::call_once(once, [] { built = build_registry(); });
    if (!built) return nullptr;
    return bg3se::TypeInformationRepository::GetInstance().TryGetType(bg3se::FixedString(name));
}

// A TypeInformationRef's target, or null for an unbound ref.
extern "C" void const* bg3le_type_info_ref(void const* ref) {
    if (ref == nullptr) return nullptr;
    auto const* r = static_cast<bg3se::TypeInformationRef const*>(ref);
    if (!*r) return nullptr;
    return &r->Get();
}

namespace {
char const* fs_text(bg3se::FixedString const& s) {
    return s ? s.GetString() : nullptr;
}

// NativeName is typeid().name(): mangled here, where MSVC gives
// "struct bg3se::HealthComponent".
char const* native_name(char const* raw) {
    if (raw == nullptr) return nullptr;
    static std::mutex lock;
    static std::unordered_map<std::string, std::string> names;
    std::lock_guard<std::mutex> guard(lock);
    auto it = names.find(raw);
    if (it != names.end()) return it->second.c_str();
    int status = 0;
    char* plain = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    std::string name = status == 0 && plain != nullptr ? std::string("struct ") + plain : raw;
    std::free(plain);
    return names.emplace(raw, std::move(name)).first->second.c_str();
}
}  // namespace

// A TypeInformation, field by field, for the Lua view over it.
struct Bg3leTypeInfo {
    char const* TypeName;
    char const* NativeName;
    char const* Kind;
    void const* KeyType;
    void const* ElementType;
    void const* ParentType;
    char const* ModuleRole;
    char const* ComponentName;
    char const* SystemName;
    bool HasWildcardProperties;
    bool VarargParams;
    bool VarargsReturn;
    bool IsBitfield;
    bool IsBuiltin;
};

extern "C" bool bg3le_type_info_fields(void const* at, Bg3leTypeInfo* out) {
    if (at == nullptr || out == nullptr) return false;
    auto const* ty = static_cast<bg3se::TypeInformation const*>(at);
    out->TypeName = fs_text(ty->TypeName);
    out->NativeName = ty->Kind == bg3se::LuaTypeId::Object && !ty->ModuleRole
                          ? native_name(fs_text(ty->NativeName)) : fs_text(ty->NativeName);
    out->Kind = fs_text(bg3se::EnumInfo<bg3se::LuaTypeId>::Find(ty->Kind));
    out->KeyType = bg3le_type_info_ref(&ty->KeyType);
    out->ElementType = bg3le_type_info_ref(&ty->ElementType);
    out->ParentType = bg3le_type_info_ref(&ty->ParentType);
    out->ModuleRole = fs_text(ty->ModuleRole);
    out->ComponentName = fs_text(ty->ComponentName);
    out->SystemName = fs_text(ty->SystemName);
    out->HasWildcardProperties = ty->HasWildcardProperties;
    out->VarargParams = ty->VarargParams;
    out->VarargsReturn = ty->VarargsReturn;
    out->IsBitfield = ty->IsBitfield;
    out->IsBuiltin = ty->IsBuiltin;
    return true;
}

// which: 0 Members, 1 Methods, 2 EnumValues, 3 Params, 4 ReturnValues.
extern "C" void bg3le_type_info_each(void const* at, int which,
                                     void (*each)(void* user, char const* key, void const* type,
                                                  unsigned long long value),
                                     void* user) {
    if (at == nullptr) return;
    auto const* ty = static_cast<bg3se::TypeInformation const*>(at);
    switch (which) {
    case 0:
        for (auto const& m : ty->Members) each(user, fs_text(m.Key), bg3le_type_info_ref(&m.Value), 0);
        break;
    case 1:
        for (auto const& m : ty->Methods) each(user, fs_text(m.Key), &m.Value, 0);
        break;
    case 2:
        for (auto const& e : ty->EnumValues) each(user, fs_text(e.Key), nullptr, e.Value);
        break;
    case 3:
        for (auto const& p : ty->Params) each(user, nullptr, bg3le_type_info_ref(&p), 0);
        break;
    case 4:
        for (auto const& r : ty->ReturnValues) each(user, nullptr, bg3le_type_info_ref(&r), 0);
        break;
    }
}

// Every registered type name, as upstream GetAllTypes.
extern "C" void bg3le_type_info_names(void (*each)(void* user, char const* name), void* user) {
    if (bg3le_type_info("bool") == nullptr) return;
    for (auto const& t : bg3se::TypeInformationRepository::GetInstance().GetAllTypes()) {
        each(user, fs_text(t.Key));
    }
}
