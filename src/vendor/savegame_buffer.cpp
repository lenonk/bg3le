// ObjectVisitor::VisitBuffer for src/savegame.cpp, which cannot see the
// vendored ScratchBuffer: a CompositeBinary user variable, as bg3se writes
// them, arrives as one.

#include <stdafx.h>

#include <cstdint>
#include <string>

extern "C" bool bg3le_savegame_read_buffer(void* visitor, int slot, std::uint32_t name,
                                           std::string* out)
{
    bg3se::ScratchBuffer buffer;
    using Fn = void (*)(void*, std::uint32_t const*, bg3se::ScratchBuffer*);
    auto visit = reinterpret_cast<Fn>((*reinterpret_cast<void***>(visitor))[slot]);
    visit(visitor, &name, &buffer);
    if (buffer.Buffer.Buffer == nullptr || buffer.Size == 0) return false;
    out->assign(static_cast<char const*>(buffer.Buffer.Buffer), (std::size_t)buffer.Size);
    return true;
}
