// The translated strings Ext.Loca reads, and what turns the loca handles
// Ext.Stats reports into text.
//
// ls::TranslatedStringRepository has no symbol, and an earlier attempt to
// find it by fingerprinting memory was not specific enough: a hash map
// whose keys are loca handles matched structures that were not text pools,
// harvesting 956 entries one run and 3 the next with no text in any of
// them. That would have made Ext.Loca answer "" for every handle.
//
// So the strings are read where the engine reads them from: the .loca
// files in the archives, the same route src/vendor/stat_origins.cpp takes
// for which mod defines a stat. Deterministic, verifiable against the game
// -- BURNING's DisplayName resolves to "Burning" -- and it works before the
// repository has been populated at all.
//
// LOCA is a flat format: a twelve-byte header of magic, entry count and the
// offset where the text begins, then that many seventy-byte entries of a
// sixty-four byte key, a version and a length, then the strings back to
// back, each NUL-terminated. 12 + 232878 * 70 is exactly the text offset
// this build's english.loca declares, which is what confirms the entry
// size rather than the format being taken on trust.

#include <stdafx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../log.h"
#include "../mem.h"
#include "../pak.h"

namespace bg3le {

namespace {

constexpr char kMagic[4] = {'L', 'O', 'C', 'A'};
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kKeySize = 64;
constexpr std::size_t kEntrySize = 70;   // key, uint16 version, uint32 length

struct Strings {
    bool Built{false};
    // The decompressed files, kept alive because the index points into
    // them: neither the keys nor the text are copied.
    std::vector<std::string> Blobs;
    std::unordered_map<std::string_view, std::string_view> ByHandle;
    std::vector<std::string_view> Order;

    // What mods have set. Kept separately because these are owned copies
    // rather than views into a blob, and because the distinction is worth
    // keeping: everything else here came out of the game's own files.
    std::vector<std::pair<std::string, std::string>> Written;
};

Strings& state() {
    static Strings s;
    return s;
}

// The language the install is configured for, from
// Data/Localization/language.lsx. Read rather than assumed so a
// non-English install resolves its own strings.
std::string configured_language(std::string const& dataRoot) {
    const std::string path = dataRoot + "/Localization/language.lsx";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return "English";

    std::string contents;
    char block[8192];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
        contents.append(block, got);
    }
    std::fclose(f);

    // <attribute id="Value" value="English" type="20" />, the entry whose
    // MapKey is Language.
    const std::size_t key = contents.find("\"Language\"");
    if (key == std::string::npos) return "English";
    const std::size_t value = contents.find("id=\"Value\" value=\"", key);
    if (value == std::string::npos) return "English";

    const std::size_t from = value + std::strlen("id=\"Value\" value=\"");
    const std::size_t to = contents.find('"', from);
    if (to == std::string::npos || to <= from) return "English";
    return contents.substr(from, to - from);
}

// Indexes one .loca file. Later files override earlier ones, which is the
// order the engine consults its pools in.
std::size_t index_loca(std::string const& blob, Strings* into) {
    if (blob.size() < kHeaderSize
        || std::memcmp(blob.data(), kMagic, sizeof(kMagic)) != 0) {
        return 0;
    }

    std::uint32_t count = 0;
    std::uint32_t textOffset = 0;
    std::memcpy(&count, blob.data() + 4, sizeof(count));
    std::memcpy(&textOffset, blob.data() + 8, sizeof(textOffset));

    // The entry table has to account for exactly the space before the
    // text; anything else means this is not the layout above.
    if ((std::size_t)textOffset != kHeaderSize + (std::size_t)count * kEntrySize
        || textOffset > blob.size()) {
        logf("loca: header does not describe %u entries; skipping", count);
        return 0;
    }

    std::size_t added = 0;
    std::size_t entry = kHeaderSize;
    std::size_t text = textOffset;
    for (std::uint32_t i = 0; i < count; ++i) {
        char const* key = blob.data() + entry;
        const std::size_t keyLength = strnlen(key, kKeySize);

        std::uint32_t length = 0;
        std::memcpy(&length, blob.data() + entry + kKeySize + 2,
                    sizeof(length));
        entry += kEntrySize;

        if (text + length > blob.size()) break;
        // Each string is NUL-terminated within its own length, so the text
        // is used as it lies rather than copied.
        char const* value = blob.data() + text;
        text += length;

        if (keyLength == 0) continue;
        into->ByHandle[std::string_view(key, keyLength)] =
            std::string_view(value, strnlen(value, length));
        ++added;
    }
    return added;
}

std::string data_root() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';

    std::string path(exe);
    const std::size_t bin = path.rfind("/bin/");
    if (bin == std::string::npos) return {};
    return path.substr(0, bin) + "/Data";
}

std::vector<std::string> paks_in(std::string const& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return out;

    while (dirent* e = readdir(d)) {
        std::string const name = e->d_name;
        if (name.size() < 5) continue;
        if (name.compare(name.size() - 4, 4, ".pak") != 0) continue;
        out.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string user_mods_directory() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home)
           + "/.local/share/Larian Studios/Baldur's Gate 3/Mods";
}

bool build() {
    const std::string root = data_root();
    if (root.empty()) return false;

    const std::string language = configured_language(root);
    // Localization/<Language>/, and not the Gender subdirectories: those
    // are per-pronoun overrides the engine keeps in separate pools and
    // selects between, not part of the base text.
    const std::string prefix = "Localization/" + language + "/";

    Strings found{};
    std::vector<std::string> archives = paks_in(root + "/Localization");
    for (std::string const& pak : paks_in(root)) archives.push_back(pak);
    for (std::string const& pak : paks_in(user_mods_directory())) {
        archives.push_back(pak);
    }

    std::size_t files = 0;
    std::size_t entries = 0;
    for (std::string const& pak : archives) {
        pak_read(
            pak.c_str(),
            [&](char const* name) {
                std::string_view path(name);
                if (path.find(prefix) == std::string_view::npos) return false;
                if (path.find("/Gender/") != std::string_view::npos) {
                    return false;
                }
                return path.size() > 5
                       && path.compare(path.size() - 5, 5, ".loca") == 0;
            },
            [&](char const*, char const* data, std::size_t size) {
                found.Blobs.emplace_back(data, size);
                ++files;
            });
    }

    // Indexed after every blob is in place: the index holds views into
    // them, and growing the vector would move them.
    for (std::string const& blob : found.Blobs) {
        entries += index_loca(blob, &found);
    }

    if (found.ByHandle.empty()) {
        logf("loca: no %s strings found in %zu archives; Ext.Loca stays "
             "unavailable", language.c_str(), archives.size());
        return false;
    }

    found.Order.reserve(found.ByHandle.size());
    for (auto const& entry : found.ByHandle) {
        found.Order.push_back(entry.first);
    }

    found.Built = true;
    state() = std::move(found);
    logf("loca: %zu %s strings from %zu .loca files (%zu entries read)",
         state().ByHandle.size(), language.c_str(), files, entries);
    return true;
}

bool ready() {
    // Only the warming thread scans; see mem.h.
    if (!state().Built && !scan_allowed()) return false;

    if (state().Built) return true;

    // The archives do not change while the game runs, so one attempt
    // settles it either way -- unlike the memory searches, this does not
    // have to wait for the engine to populate anything.
    static bool tried = false;
    if (tried) return false;
    tried = true;
    return build();
}

}  // namespace

extern "C" bool bg3le_loca_ready() { return ready(); }

bool translated_string_get(char const* handle, std::string* out);
bool translated_string_set(char const* handle, char const* text);

// The engine's repository first, as upstream reads it; the index when the
// repository is not up yet.
extern "C" char const* bg3le_loca_get(char const* handle) {
    if (handle == nullptr) return nullptr;
    thread_local std::string fromRepository;
    if (translated_string_get(handle, &fromRepository)) {
        return fromRepository.c_str();
    }
    if (!ready()) return nullptr;

    auto it = state().ByHandle.find(std::string_view(handle));
    if (it == state().ByHandle.end()) return nullptr;
    // NUL-terminated where it lies, inside the blob or in the copy a mod
    // wrote.
    return it->second.data();
}

// What Ext.Loca.UpdateTranslatedString writes: the engine's repository, as
// upstream does, so the interface shows it; and this index, so reads made
// before the repository is up agree with it.
extern "C" bool bg3le_loca_set(char const* handle, char const* text) {
    if (handle == nullptr || text == nullptr || handle[0] == '\0') {
        return false;
    }
    translated_string_set(handle, text);
    // Build first, because building replaces the whole index and would
    // throw away anything written before it.
    ready();

    Strings& strings = state();
    strings.Written.emplace_back(handle, text);
    auto const& stored = strings.Written.back();

    const std::string_view key(stored.first);
    const std::string_view value(stored.second);
    auto it = strings.ByHandle.find(key);
    if (it == strings.ByHandle.end()) {
        strings.ByHandle.emplace(key, value);
        strings.Order.push_back(key);
    } else {
        it->second = value;
    }
    return true;
}

extern "C" std::size_t bg3le_loca_count() {
    return ready() ? state().ByHandle.size() : 0;
}

extern "C" char const* bg3le_loca_handle_at(std::size_t index) {
    if (!ready() || index >= state().Order.size()) return nullptr;
    return state().Order[index].data();
}

}  // namespace bg3le
