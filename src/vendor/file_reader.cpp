// A FileReader bg3le fills in itself, for the paths upstream reads through
// the engine's virtual file system.
//
// StaticSymbols::MakeFileReader needs ls::FileReader's constructor, and the
// Linux build exports no engine functions by name, so every upstream caller
// of script::LoadExternalFile(.., PathRootType::Data) got "File reader API
// not available!" -- including IMGUIManager::LoadFont, which left the font
// atlas empty. Norbyte's imgui fork has AddFontDefault() disabled, so an
// empty atlas is not a smaller overlay, it is no overlay at all: nothing
// draws.
//
// FileReader is a plain struct, though, and the callers read three fields of
// it. So bg3le reads the archives (see game_files.h) and hands back a reader
// pointing at its own buffer, marked so DestroyFileReader can tell whose it
// is.

#include <stdafx.h>

#include <GameDefinitions/FileReader.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "../game_files.h"

namespace bg3le {

namespace {

// In FileHandle, which nothing here uses and the engine never sees.
constexpr std::uint64_t kOwnedByBg3le = 0x6267336c65524452ull;  // bg3leRDR

}  // namespace

// Null if the path is not in the game's data, which is the same answer
// upstream gives for a reader that failed to load.
bg3se::FileReader* make_data_file_reader(std::string_view path) {
    std::string body;
    // The game's archives, then the mods' own, as the engine's VFS mounts both.
    const std::string name(path);
    if (!game_file_read(name.c_str(), &body) && !mod_file_read(name.c_str(), &body)) {
        return nullptr;
    }

    auto* owned = new std::string(std::move(body));

    // Allocated raw and zeroed rather than constructed: FileReader holds a
    // ScratchBuffer whose destructor would run on a struct the engine never
    // filled in. Zero is every member's default, and bg3le owns this
    // allocation outright, so it is ours to free.
    auto* reader = static_cast<bg3se::FileReader*>(
        std::calloc(1, sizeof(bg3se::FileReader)));
    if (reader == nullptr) {
        delete owned;
        return nullptr;
    }

    reader->IsLoaded = true;
    reader->DataPtr = owned->data();
    reader->ReadPtr = owned->data();
    reader->FileSize = owned->size();
    reader->FileHandle = kOwnedByBg3le;
    reader->FileObject = owned;
    reader->Type = bg3se::FileType::MemBuffer;
    return reader;
}

// False if this reader is not one of ours, so the caller can fall through to
// the engine's destructor.
bool destroy_data_file_reader(bg3se::FileReader* reader) {
    if (reader == nullptr || reader->FileHandle != kOwnedByBg3le) return false;

    delete static_cast<std::string*>(reader->FileObject);
    std::free(reader);
    return true;
}

}  // namespace bg3le
