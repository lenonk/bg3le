#include "ecs_types.h"

#include <cxxabi.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "mem.h"

namespace bg3le {
namespace ecs {
namespace {

// _ZN2ls6TypeIdI<T><Context>E11m_TypeIndexE
constexpr char kPrefix[] = "_ZN2ls6TypeIdI";
constexpr char kSuffix[] = "11m_TypeIndexE";

struct Registry {
    std::array<std::unordered_map<std::string, const std::int32_t*>,
               static_cast<std::size_t>(Context::Other) + 1>
        by_context;
    bool loaded = false;
};

Registry& registry() {
    static Registry r;
    return r;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
}

// Splits "A, B" at the comma that is not inside angle brackets. Template
// arguments nest arbitrarily deep -- ecs::query::spec::Spec<ls::TypeList<...>>
// and friends -- so counting is the only way to find the top level.
bool split_template_args(const std::string& args, std::string* first,
                         std::string* second) {
    int depth = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const char c = args[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            --depth;
        } else if (c == ',' && depth == 0) {
            *first = args.substr(0, i);
            std::size_t start = i + 1;
            while (start < args.size() && args[start] == ' ') ++start;
            *second = args.substr(start);
            return true;
        }
    }
    return false;
}

Context classify(const std::string& context) {
    if (context == "ecs::ComponentTypeIdContext") return Context::Component;
    if (context == "ecs::OneFrameComponentTypeIdContext") {
        return Context::OneFrameComponent;
    }
    if (context == "ecs::SystemsContext") return Context::System;
    if (context == "ecs::sync::ReplicatedTypeContext") return Context::Replication;
    if (context == "ls::ImmutableDataHeadmaster") return Context::ImmutableData;
    if (context == "ecs::UncheckedComponentTypeIdContext") return Context::Unchecked;
    return Context::Other;
}

// "ls::TypeId<A, B>::m_TypeIndex" -> A and B.
bool parse_demangled(const char* demangled, std::string* type,
                     std::string* context) {
    const std::string s(demangled);
    const std::size_t open = s.find('<');
    const std::size_t close = s.rfind(">::m_TypeIndex");
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return false;
    }
    return split_template_args(s.substr(open + 1, close - open - 1), type, context);
}

}  // namespace

const char* context_name(Context context) {
    switch (context) {
        case Context::Component:         return "Component";
        case Context::OneFrameComponent: return "OneFrameComponent";
        case Context::System:            return "System";
        case Context::Replication:       return "Replication";
        case Context::ImmutableData:     return "ImmutableData";
        case Context::Unchecked:         return "Unchecked";
        default:                         return "Other";
    }
}

std::size_t load(const SymbolTable& symbols) {
    Registry& reg = registry();
    if (reg.loaded) {
        std::size_t total = 0;
        for (const auto& m : reg.by_context) total += m.size();
        return total;
    }
    reg.loaded = true;

    std::size_t total = 0;
    std::size_t undemangled = 0;

    symbols.for_each([&](const std::string& mangled, std::uintptr_t addr) {
        if (mangled.compare(0, std::strlen(kPrefix), kPrefix) != 0) return;
        if (!ends_with(mangled, kSuffix)) return;

        int status = 0;
        char* demangled = ::abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr,
                                                &status);
        if (demangled == nullptr || status != 0) {
            std::free(demangled);
            ++undemangled;
            return;
        }

        std::string type;
        std::string context;
        if (parse_demangled(demangled, &type, &context)) {
            const auto slot = static_cast<std::size_t>(classify(context));
            reg.by_context[slot].emplace(std::move(type),
                                         reinterpret_cast<const std::int32_t*>(addr));
            ++total;
        }
        std::free(demangled);
    });

    logf("ecs: %zu type indices from the symbol table (%zu components, "
         "%zu one-frame, %zu systems, %zu replicated, %zu static data)",
         total, count(Context::Component), count(Context::OneFrameComponent),
         count(Context::System), count(Context::Replication),
         count(Context::ImmutableData));
    if (undemangled > 0) {
        logf("ecs: %zu type index symbols could not be demangled", undemangled);
    }
    return total;
}

std::optional<std::int32_t> index_of(Context context, const std::string& name) {
    const auto& m = registry().by_context[static_cast<std::size_t>(context)];
    const auto it = m.find(name);
    if (it == m.end()) return std::nullopt;

    // Read through the fault-tolerant path: these live in .bss, so a wrong
    // address returns nothing rather than taking the game down.
    std::int32_t value = 0;
    if (!safe_read(it->second, &value, sizeof(value))) return std::nullopt;

    // The engine assigns indices during startup; -1 means not yet assigned.
    if (value < 0) return std::nullopt;
    return value;
}

std::size_t count(Context context) {
    return registry().by_context[static_cast<std::size_t>(context)].size();
}

// These walk the context rather than keeping a reverse map, because the
// indices are read live: a cached reverse map would go stale the moment the
// engine assigned one. They are for identifying a structure once, not for a
// hot path.
bool has_index(Context context, std::int32_t index) {
    if (index < 0) return false;
    const auto& m = registry().by_context[static_cast<std::size_t>(context)];
    for (const auto& entry : m) {
        std::int32_t value = 0;
        if (!safe_read(entry.second, &value, sizeof(value))) continue;
        if (value == index) return true;
    }
    return false;
}

namespace {

// index -> name for one context, built from a live read. An index is
// assigned once at startup and never changes, so the map only needs
// rebuilding while some are still unassigned.
struct ReverseMap {
    std::vector<const std::string*> byIndex;
    bool complete = false;
};

ReverseMap& reverse(Context context) {
    static std::array<ReverseMap, 7> maps;
    auto& r = maps[static_cast<std::size_t>(context)];
    if (r.complete) return r;

    r.byIndex.clear();
    bool complete = true;
    for (const auto& entry : registry().by_context[static_cast<std::size_t>(context)]) {
        std::int32_t value = -1;
        if (!safe_read(entry.second, &value, sizeof(value)) || value < 0) {
            complete = false;
            continue;
        }
        if ((std::size_t)value >= r.byIndex.size()) {
            r.byIndex.resize((std::size_t)value + 1, nullptr);
        }
        r.byIndex[(std::size_t)value] = &entry.first;
    }
    r.complete = complete;
    return r;
}

}  // namespace

std::optional<std::string> name_of(Context context, std::int32_t index) {
    if (index < 0) return std::nullopt;
    auto& r = reverse(context);
    if ((std::size_t)index < r.byIndex.size() && r.byIndex[(std::size_t)index]) {
        return *r.byIndex[(std::size_t)index];
    }
    return std::nullopt;
}

std::vector<std::pair<std::int32_t, std::string>> assigned(Context context) {
    std::vector<std::pair<std::int32_t, std::string>> out;
    auto& r = reverse(context);
    for (std::size_t i = 0; i < r.byIndex.size(); ++i) {
        if (r.byIndex[i]) out.emplace_back((std::int32_t)i, *r.byIndex[i]);
    }
    return out;
}

}  // namespace ecs
}  // namespace bg3le
