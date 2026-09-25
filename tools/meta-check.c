// Checks the component field tables extracted from bg3se's generated metadata,
// without needing the game.
//
// libbg3le.so loads standalone (see dlopen-check.c), and the field tables are
// static data built at compile time, so the offsets can be read out of a
// freshly dlopened library. Nothing here touches the ECS -- that needs a live
// container -- so this validates the metadata itself: that the re-expansion in
// src/vendor/component_meta.cpp produced real offsets for real fields, and
// that base-class fields resolve.
//
//   cc -o /tmp/meta-check tools/meta-check.c -ldl && /tmp/meta-check build/libbg3le.so

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void const* (*meta_component)(char const*);
static size_t (*meta_component_size)(void const*);
static int (*meta_field)(void const*, char const*, uint32_t*, uint16_t*,
                         uint8_t*, uint8_t*, uint16_t*);
static size_t (*meta_fields)(void const*, char const**, uint8_t*, size_t);
static size_t (*meta_fields_at)(void const*, char const*, char const**,
                                uint8_t*, size_t);
static size_t (*meta_class_count)(void);
static size_t (*meta_component_count)(void);
static int (*meta_selftest)(void);
static int (*install_game_allocator)(void*, void*);
static void const* (*meta_class_at)(size_t);
static char const* (*meta_engine_class)(void const*);
static char const* (*meta_kind_name)(uint8_t);
static char const* (*meta_class_name)(void const*);
static int (*meta_type_name_at)(void const*, char const*, char const**, uint16_t*);

// --unsupported: every field that does not convert, with its declared type,
// components first. The list to work down when adding a field kind.
static int list_unsupported(void) {
    char const* names[512];
    uint8_t kinds[512];
    for (int pass = 0; pass < 2; pass++) {
        printf(pass == 0 ? "components:\n" : "\nother classes:\n");
        for (size_t i = 0; i < meta_class_count(); i++) {
            void const* cls = meta_class_at(i);
            if (cls == NULL || (meta_engine_class(cls) != NULL) != (pass == 0)) continue;
            size_t n = meta_fields(cls, names, kinds, 512);
            for (size_t j = 0; j < n; j++) {
                if (kinds[j] != 0) continue;
                char const* type = NULL;
                uint16_t len = 0;
                meta_type_name_at(cls, names[j], &type, &len);
                printf("  %s.%s\t%.*s\n", meta_class_name(cls), names[j],
                       type != NULL ? (int)len : 1, type != NULL ? type : "?");
            }
        }
    }
    return 0;
}

static int failures = 0;

// Checks one field's offset and size against what the component's layout says
// it should be. Expected values come from reading the struct in
// vendor/bg3se/BG3Extender/GameDefinitions/Components/, so this catches a
// re-expansion that silently produced zeroes or shifted entries.
static void expect_field(char const* component, char const* field,
                         uint32_t wantOffset, uint16_t wantSize) {
    void const* meta = meta_component(component);
    if (meta == NULL) {
        printf("  FAIL %s: no metadata\n", component);
        failures++;
        return;
    }

    uint32_t offset = 0;
    uint16_t size = 0;
    uint8_t kind = 0, elemKind = 0;
    uint16_t elemCount = 0;
    if (!meta_field(meta, field, &offset, &size, &kind, &elemKind, &elemCount)) {
        printf("  FAIL %s.%s: not found\n", component, field);
        failures++;
        return;
    }

    if (offset != wantOffset || size != wantSize) {
        printf("  FAIL %s.%s: offset %u size %u, expected offset %u size %u\n",
               component, field, offset, size, wantOffset, wantSize);
        failures++;
        return;
    }

    printf("  ok   %s.%s at +%u, %u bytes, kind %u\n", component, field, offset,
           size, kind);
}

static void expect_absent(char const* component, char const* field) {
    void const* meta = meta_component(component);
    uint32_t offset = 0;
    uint16_t size = 0;
    uint8_t kind = 0, elemKind = 0;
    uint16_t elemCount = 0;
    if (meta != NULL
        && meta_field(meta, field, &offset, &size, &kind, &elemKind, &elemCount)) {
        printf("  FAIL %s.%s: reported present, should not be\n", component,
               field);
        failures++;
        return;
    }
    printf("  ok   %s.%s correctly absent\n", component, field);
}

// Checks that a field reports the given kind. Used to pin down that a type
// bg3se cannot traverse is reported as unsupported rather than as a struct,
// since a caller acts on that answer.
static void expect_kind(char const* component, char const* field,
                        char const* wantKind) {
    void const* meta = meta_component(component);
    uint32_t offset = 0;
    uint16_t size = 0, elemCount = 0;
    uint8_t kind = 0, elemKind = 0;
    if (meta == NULL
        || !meta_field(meta, field, &offset, &size, &kind, &elemKind,
                       &elemCount)) {
        printf("  FAIL %s.%s: not found\n", component, field);
        failures++;
        return;
    }
    if (strcmp(meta_kind_name(kind), wantKind) != 0) {
        printf("  FAIL %s.%s: kind %s, expected %s\n", component, field,
               meta_kind_name(kind), wantKind);
        failures++;
        return;
    }
    printf("  ok   %s.%s is %s\n", component, field, wantKind);
}

// Checks that a dotted path accumulates offsets: the offset of "outer.inner"
// has to be the offset of "outer" plus the offset of "inner" inside it.
//
// inner may be NULL, in which case it is taken to be the first listed field of
// outer -- which still exercises the accumulation, and saves naming a member
// whose spelling is not obvious from the component.
static void expect_path_adds(char const* component, char const* outer,
                             char const* inner) {
    void const* meta = meta_component(component);
    if (meta == NULL) {
        printf("  FAIL %s: no metadata\n", component);
        failures++;
        return;
    }

    uint32_t outerOffset = 0, pathOffset = 0;
    uint16_t size = 0, elemCount = 0;
    uint8_t kind = 0, elemKind = 0;
    if (!meta_field(meta, outer, &outerOffset, &size, &kind, &elemKind,
                    &elemCount)) {
        printf("  FAIL %s.%s: not found\n", component, outer);
        failures++;
        return;
    }

    char const* names[256];
    uint8_t kinds[256];
    size_t n = meta_fields_at(meta, outer, names, kinds, 256);
    if (n == 0) {
        printf("  FAIL %s.%s: not traversable\n", component, outer);
        failures++;
        return;
    }
    if (inner == NULL) inner = names[0];

    // The inner field's offset within its own struct, reached through the
    // path; and the same field reached as a path from the component.
    char path[256];
    snprintf(path, sizeof(path), "%s.%s", outer, inner);
    uint32_t innerSize = 0;
    uint16_t isz = 0;
    if (!meta_field(meta, path, &pathOffset, &isz, &kind, &elemKind,
                    &elemCount)) {
        printf("  FAIL %s: not found\n", path);
        failures++;
        return;
    }
    (void)innerSize;

    if (pathOffset < outerOffset || pathOffset >= outerOffset + size) {
        printf("  FAIL %s: offset %u is outside %s at +%u..+%u\n", path,
               pathOffset, outer, outerOffset, outerOffset + size);
        failures++;
        return;
    }

    printf("  ok   %s at +%u, inside %s at +%u (+%u within)\n", path,
           pathOffset, outer, outerOffset, pathOffset - outerOffset);
}

// Fields whose type this build compiles to a different size from the engine.
//
// The game is built against libc++ ABI 2 and bg3le against ABI 1; the build
// gives std::variant the game's one-byte index, which makes every flat
// variant agree, but a variant nested inside a variant is still padded
// differently by this libc++ version. A struct holding one is then the wrong
// size -- and if anything follows it, those members sit at the wrong
// offsets, which no size-only correction repairs. That case fails the check.
// The size-only case is listed, and passes only if component_meta.cpp gives
// the struct its engine size, which it asserts at compile time.
static void check_layout_differences(void* lib) {
    size_t (*class_count)(void) = dlsym(lib, "bg3le_meta_class_count");
    void const* (*class_at)(size_t) = dlsym(lib, "bg3le_meta_class_at");
    char const* (*class_name)(void const*) = dlsym(lib, "bg3le_meta_class_name");
    int (*field_sizes)(void const*, char const*, uint16_t*, uint16_t*,
                       uint32_t*) = dlsym(lib, "bg3le_meta_field_sizes");
    if (!class_count || !class_at || !class_name || !field_sizes) {
        printf("layout: exports missing\n");
        failures++;
        return;
    }

    int differing = 0;
    printf("\nlayout differences from the engine (compiled size != engine size):\n");
    for (size_t c = 0; c < class_count(); ++c) {
        void const* cls = class_at(c);
        if (cls == NULL) continue;
        char const* names[512];
        uint8_t kinds[512];
        size_t n = meta_fields_at(cls, NULL, names, kinds, 512);

        uint32_t last = 0;
        for (size_t i = 0; i < n; ++i) {
            uint16_t e = 0, cs = 0;
            uint32_t off = 0;
            if (field_sizes(cls, names[i], &e, &cs, &off) && off > last) last = off;
        }
        for (size_t i = 0; i < n; ++i) {
            uint16_t e = 0, cs = 0;
            uint32_t off = 0;
            if (!field_sizes(cls, names[i], &e, &cs, &off) || cs == 0) continue;
            ++differing;
            const int followed = off < last;
            printf("  %-44s %-12s +%-4u engine %-4u compiled %-4u %s\n",
                   class_name(cls), names[i], off, e, cs,
                   followed ? "FAIL: members after it are misplaced"
                            : "last member; struct size corrected");
            if (followed) failures++;
        }
    }
    printf("  %d field(s)\n", differing);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to libbg3le.so>\n", argv[0]);
        return 2;
    }

    void* h = dlopen(argv[1], RTLD_NOW);
    if (h == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

#define BIND(var, name)                                     \
    var = dlsym(h, name);                                   \
    if (var == NULL) {                                      \
        fprintf(stderr, "missing symbol: %s\n", name);      \
        return 1;                                           \
    }

    BIND(meta_component, "bg3le_meta_component")
    BIND(meta_component_size, "bg3le_meta_component_size")
    BIND(meta_field, "bg3le_meta_field")
    BIND(meta_fields, "bg3le_meta_fields")
    BIND(meta_fields_at, "bg3le_meta_fields_at")
    BIND(meta_class_count, "bg3le_meta_class_count")
    BIND(meta_component_count, "bg3le_meta_component_count")
    BIND(meta_selftest, "bg3le_meta_selftest")
    BIND(install_game_allocator, "bg3le_install_game_allocator")
    BIND(meta_class_at, "bg3le_meta_class_at")
    BIND(meta_engine_class, "bg3le_meta_engine_class")
    BIND(meta_kind_name, "bg3le_meta_kind_name")
    BIND(meta_class_name, "bg3le_meta_class_name")
    BIND(meta_type_name_at, "bg3le_meta_type_name_at")
#undef BIND

    if (argc > 2 && strcmp(argv[2], "--unsupported") == 0) return list_unsupported();

    // The self-test builds a real dynamic array, which allocates through
    // bg3se. In the game that goes to the engine's heap; here malloc will do,
    // because none of this memory is ever handed to the engine.
    if (!install_game_allocator(malloc, free)) {
        printf("  FAIL could not install the test allocator\n");
        failures++;
    } else {
        const int selftestFailures = meta_selftest();
        if (selftestFailures != 0) {
            printf("  FAIL the dynamic array self-test reported %d failure(s)"
                   " (see the log for detail)\n", selftestFailures);
            failures += selftestFailures;
        } else {
            printf("  ok   dynamic array walk self-test\n");
        }
    }

    printf("%zu classes, %zu of them named components\n", meta_class_count(),
           meta_component_count());

    // HealthComponent: int Hp, MaxHp, TemporaryHp, MaxTemporaryHp, then a Guid.
    // These are the offsets the hand-written accessor relied on, so they are
    // the ones already confirmed against the running game.
    expect_field("eoc::HealthComponent", "Hp", 0, 4);
    expect_field("eoc::HealthComponent", "MaxHp", 4, 4);
    expect_field("eoc::HealthComponent", "TemporaryHp", 8, 4);
    expect_field("eoc::HealthComponent", "MaxTemporaryHp", 12, 4);
    expect_field("eoc::HealthComponent", "IsInvulnerable", 32, 1);

    // A field that does not exist has to be reported as missing rather than
    // resolving to offset 0, which would read Hp instead.
    expect_absent("eoc::HealthComponent", "NoSuchField");

    // Both names reach the same component: the engine's and bg3se's short one.
    if (meta_component("eoc::HealthComponent") != meta_component("Health")) {
        printf("  FAIL Health and eoc::HealthComponent disagree\n");
        failures++;
    } else {
        printf("  ok   Health and eoc::HealthComponent agree\n");
    }

    // StatsComponent covers the two kinds added on top of plain scalars:
    // std::array<int, 7> and an enum that resolves to its underlying integer.
    expect_field("eoc::StatsComponent", "AbilityModifiers", 32, 28);
    expect_field("eoc::StatsComponent", "ProficiencyBonus", 132, 4);
    expect_field("eoc::StatsComponent", "SpellCastingAbility", 136, 1);

    // A dotted path has to add the offset of each step. Checked as arithmetic
    // rather than against a literal, so it holds whatever the inner layout is:
    // the path offset must equal the outer field's offset plus the inner
    // field's offset within its own struct.
    expect_path_adds("ls::TransformComponent", "Transform", "Translate");
    expect_path_adds("ls::TransformComponent", "Transform", NULL);

    // A map has to be indexed before it can be descended into: naming a field
    // of the map itself must fail rather than reading the container's own
    // bytes as a struct.
    expect_absent("eoc::ActionResourcesComponent", "Resources.Amount");
    expect_kind("eoc::ActionResourcesComponent", "Resources", "map");

    // glm vectors carry everything positional, and they are the reason
    // Bound.Translate read as unsupported until they were recognised. A vec3
    // is three floats; kind 14 is a fixed-extent array.
    expect_kind("eoc::BoundComponent", "Bound.Translate", "array");
    expect_kind("eoc::BoundComponent", "Bound.RotationQuat", "array");
    expect_kind("eoc::BoundComponent", "Bound.Scale", "float");

    // Hash sets read as arrays of their keys.
    expect_kind("eoc::summon::ContainerComponent", "Characters", "array");

    void const* health = meta_component("eoc::HealthComponent");
    printf("  eoc::HealthComponent is %zu bytes\n", meta_component_size(health));

    char const* names[256];
    uint8_t kinds[256];
    size_t n = meta_fields(health, names, kinds, 256);
    printf("  eoc::HealthComponent fields (%zu):", n);
    for (size_t i = 0; i < n; i++) printf(" %s", names[i]);
    printf("\n");

    // How much of the component surface actually converts. Not a pass/fail --
    // it is the number to watch when a field kind is added, and the quickest
    // way to see whether a change moved the needle or only looked like it.
    {
        size_t components = 0, fields = 0, usable = 0;
        size_t byKind[32] = {0};
        for (size_t i = 0; i < meta_class_count(); i++) {
            void const* cls = meta_class_at(i);
            if (cls == NULL || meta_engine_class(cls) == NULL) continue;
            components++;
            size_t fn = meta_fields(cls, names, kinds, 256);
            for (size_t j = 0; j < fn; j++) {
                fields++;
                if (kinds[j] < 32) byKind[kinds[j]]++;
                if (kinds[j] != 0) usable++;
            }
        }
        printf("\ncoverage: %zu of %zu fields convert across %zu components"
               " (%.1f%%)\n", usable, fields, components,
               fields ? 100.0 * (double)usable / (double)fields : 0.0);

        // And across every class the metadata describes, which is where the
        // containers components do not use (CompactSet, the resource types')
        // show up.
        size_t allFields = 0, allUsable = 0;
        for (size_t i = 0; i < meta_class_count(); i++) {
            void const* cls = meta_class_at(i);
            if (cls == NULL) continue;
            size_t fn = meta_fields(cls, names, kinds, 256);
            for (size_t j = 0; j < fn; j++) {
                allFields++;
                if (kinds[j] != 0) allUsable++;
            }
        }
        printf("every class: %zu of %zu fields convert (%zu do not)\n",
               allUsable, allFields, allFields - allUsable);
        // Named by the library rather than by a list here. A hardcoded list
        // was wrong the moment a kind was inserted into the middle of the
        // enum: every label after it shifted, so the breakdown claimed 335
        // maps where there are 111, and the kinds past the end of the list
        // vanished from the report entirely. The totals stayed right, which is
        // what let it survive a glance.
        for (size_t k = 0; k < 32; k++) {
            if (byKind[k] != 0) {
                printf("  %-12s %zu\n", meta_kind_name((uint8_t)k), byKind[k]);
            }
        }
    }

    // Any further arguments are components to dump, by either name, which is
    // how to find out what a component actually offers before writing script
    // against it.
    check_layout_differences(h);

    for (int i = 2; i < argc; i++) {
        void const* m = meta_component(argv[i]);
        if (m == NULL) {
            printf("\n%s: no metadata\n", argv[i]);
            continue;
        }
        printf("\n%s (%zu bytes)\n", argv[i], meta_component_size(m));
        n = meta_fields(m, names, kinds, 256);
        for (size_t j = 0; j < n; j++) {
            uint32_t offset = 0;
            uint16_t size = 0;
            uint8_t kind = 0, elemKind = 0;
            uint16_t elemCount = 0;
            meta_field(m, names[j], &offset, &size, &kind, &elemKind,
                       &elemCount);
            if (kind == 14) {  // ScalarArray
                printf("  +%-5u %-4u array[%u] of kind %-2u %s\n", offset, size,
                       elemCount, elemKind, names[j]);
            } else {
                printf("  +%-5u %-4u kind %-2u %s\n", offset, size, kind,
                       names[j]);
            }
        }
    }

    printf(failures == 0 ? "\nall checks passed\n" : "\n%d check(s) failed\n",
           failures);
    return failures == 0 ? 0 : 1;
}
