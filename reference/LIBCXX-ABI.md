# The game is built against libc++ ABI version 2

Written 2026-09-24. Check this first when a standard-library type reads
wrong.

Every standard-library symbol in `bin/bg3` is in the inline namespace
`std::__2` — 2,803 of them, and none in `std::__1` or libstdc++'s
`__cxx11`. That is libc++ built with `_LIBCPP_ABI_VERSION=2`. bg3le builds
against the stable ABI 1, because the `libc++.a` there is to link is ABI 1,
and linking against a different ABI's compiled code is not an option.

For almost everything bg3le reads the two agree. Larian's own string is
`STDString`, not `std::string`, and their containers are their own. The
exception is `std::variant`, which the game holds inline in 55 places across
static data, components, templates and stats.

## What differs

**The index.** ABI 2 stores a variant's index in one byte, straight after
the union. ABI 1 stores four. Two consequences:

- a variant whose union is small is a different size — three one-byte
  alternatives are 2 bytes in the engine and 8 here — so a struct holding
  one inline has every later member at a different offset
- reading the index through `index()` reads the engine's one real byte plus
  three bytes of whatever the allocation last held. For a nested variant in
  a stats expression that came back as 1818322177 — `Axal`, stat file text —
  where the engine had written 1

**Nesting.** This libc++ version pads a variant held inside another variant
by eight bytes, under either ABI. The game's version does not.
`StatsExpressionInternal::Param` is 32 bytes in the engine and 40 here; so
is `GlobalConfigParameter::Value`.

## What bg3le does about it

**The index is fixed at compile time.** libc++ keeps the one-byte index
behind `_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION`, which only `<variant>`
reads and which touches nothing compiled into `libc++.a`. `CMakeLists.txt`
defines it for everything. Measured against a genuine ABI-2 configuration —
`__config_site` shadowed with version 2, syntax-only, so nothing links — it
gives the same size for every flat variant, including the tiny one above.

It has to apply to every piece of C++ in the library. protobuf-lite's
`FailDynamicCast` takes a `std::variant` by value, abseil aliases
`absl::variant` to `std::variant`, and both leave weak template
instantiations whose mangled names are identical under either layout. So
`tools/fetch-externals.sh` builds them with the same flag and writes a
stamp, and CMake refuses to configure against externals built with anything
else.

**Nesting is supplied from measurement.** No macro touches it — it differs
between libc++ versions, not ABIs, and the flag test showed no change. So
`EngineLayout<std::variant<Ts...>>` in `src/vendor/component_meta.cpp`
states the engine's rule: the union rounded to the alternatives' alignment,
the index in one byte after it, padding to the same alignment, with each
alternative's size taken from its own engine layout rather than from
`sizeof`. Two measurements pin the rule as `static_assert`s:

    Param     size 32, index at +24   (a 52-character expression's fifteen
                                       parameters, and "Placeholder0" decoding
                                       to upstream's ["Placeholder", 0])
    Variant2  size 24, index at +16   (the "Axal" read above)

Every variant is read through that rule — never `index()` or `std::visit`,
which are this libc++'s.

**Structs holding a nested variant are found, not guessed.**
`tools/meta-check.c` lists every field whose compiled size differs from the
engine's. There are three, each the last member of its struct:

    GlobalConfigParameter                 Value   engine 32, compiled 40
    esv::spell_cast::PreviewSetRequest    Param   engine 128, compiled 136
    esv::spell_cast::SystemEvent          Args    engine 944, compiled 952

Because each is last, every member still sits where `offsetof` says and only
the struct's size is wrong — which is what an array of them strides by.
`BG3LE_ENGINE_SIZE_LAST_MEMBER` gives each its engine size, and asserts at
compile time that the member really is last. If a future bg3se adds a member
after one, the build stops; if a new difference appears with members after
it, `meta-check` fails.

## How it was found

`GlobalConfigParameter` is the one that faulted. Root templates hold an
`Array` of them, and once `OverrideableProperty` fields became readable the
snapshot descended into it. At 56 bytes a stride rather than 48, the fifth
element was read from inside the fourth: a variant index of 0 where the
engine had 1, an "array" count of 1138, and a fault on the first element.
Bisecting templates, then fields, then the deep descent path by path, is
what pinned it to `ScriptConfigGlobalParameters[4].Value[0][0]`.

One wrong turn is worth recording. The first attempt to measure ABI 2 passed
`-D_LIBCPP_ABI_VERSION=2`, got numbers identical to ABI 1 everywhere, and
concluded that ABI 2 pads nested variants too. It had measured nothing:
`__config_site` pins the version and overrode the flag. The tiny variant
coming out at 8 bytes under "ABI 2" is what gave it away.
