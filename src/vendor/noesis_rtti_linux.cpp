// Stand-in C++ RTTI for the Noesis types the vendored bg3se code references.
//
// bg3se reaches Noesis types through typeid and dynamic_cast in its generated
// property-map metadata, which on Windows resolves against the Noesis import
// library. Nothing on Linux can satisfy those symbols:
//
//   - the SDK tools/fetch-externals.sh downloads is headers plus Windows .lib
//     files, so there is no Linux Noesis library to link;
//   - the native game carries no Noesis typeinfo either. It has 28,992 Noesis
//     symbols in .symtab and not one typeinfo, because Noesis uses its own
//     reflection system (Reflection::RegisterType, TypeClass) rather than C++
//     RTTI, so it is almost certainly built with RTTI disabled.
//
// Declaring them weak does not work: the references from the generated
// metadata are strong, and a strong undefined reference wins, so the library
// fails to load with
//
//   symbol lookup error: undefined symbol: _ZTIN6Noesis16DispatcherObjectE
//
// So they are defined instead, every one aliased to a single real typeinfo
// belonging to the placeholder class below. That matters for safety rather
// than tidiness: an alias to a genuine __class_type_info means a dynamic_cast
// that unexpectedly runs compares against a valid object and fails to match,
// returning nullptr, instead of dereferencing a null vptr and crashing.
//
// It is still wrong semantically -- every Noesis type appears to be the same
// type -- and it is only acceptable because no RTTI is performed on a Noesis
// type while Ext.ClientUI is off. Before that module is enabled, the real fix
// is to keep the Noesis types out of the generated property maps; they come
// from make_property_map.py scanning headers, so it is a generator-input
// change rather than a shim.

namespace bg3le {

// A virtual destructor defined out of line makes this the key function, so the
// compiler emits the vtable and typeinfo here rather than nowhere.
struct NoesisRttiPlaceholder {
    virtual ~NoesisRttiPlaceholder();
};

NoesisRttiPlaceholder::~NoesisRttiPlaceholder() = default;

}  // namespace bg3le

// Weak, not global: the vendored code does emit some of these itself, where a
// class's key function happens to be instantiated in one of its translation
// units. A weak alias yields to a real definition and only fills the gaps.
#define BG3LE_ALIAS_TYPEINFO(mangled)   \
    ".weak " mangled "\n"               \
    ".set " mangled ", _ZTIN5bg3le21NoesisRttiPlaceholderE\n"

__asm__(BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis10BaseObjectE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis10BoxedValueE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis11BaseCommandE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis24BaseObservableCollectionE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis11RoutedEventE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis12TypeMetaDataE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis12TypePropertyE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis13BaseComponentE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis13UIElementDataE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis14DependencyDataE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis16DependencyObjectE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis16DispatcherObjectE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis16FrameworkElementE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis18DependencyPropertyE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis18LuaDelegateCommandE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis4TypeE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis5PanelE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis6VisualE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis8TypeMetaE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis9TypeClassE")
        BG3LE_ALIAS_TYPEINFO("_ZTIN6Noesis9UIElementE"));
