#pragma once

// Larian's string, as the native Linux build lays it out.
//
// bg3se defines STDString as std::basic_string with the game allocator,
// which is right on Windows: there Larian's string is MSVC's std::string,
// thirty-two bytes. This build's is sixteen, and not any std::string --
// libc++'s is twenty-four. Up to fifteen characters live inline with the
// length in the last byte; past that the last byte's top bit is set and the
// object is a pointer, a size and a capacity.
//
// Proven twice over against the running game: "Shared" sits inline at
// ModuleInfo+32 with its length at +47, which is the only way the 240-byte
// Module stride adds up, and the 3,125 entries of RPGStats::Conditions read
// back as valid condition expressions at sixteen-byte spacing.
//
// This is a layout fix, not a behaviour change: the API below is the subset
// of std::basic_string the vendored code uses, with the same semantics.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <iosfwd>
#include <iterator>
#include <ostream>
#include <string_view>

BEGIN_SE()

template <class T>
class LSStringBase
{
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = T const&;
    using pointer = T*;
    using const_pointer = T const*;
    using iterator = T*;
    using const_iterator = T const*;

    static constexpr size_type npos = (size_type)-1;

    // Fifteen characters and a length byte, or a pointer, a size and a
    // capacity whose top bit says which form this is.
    // The last byte holds the length and the slot before it the terminator,
    // so fourteen characters fit inline, not fifteen.
    static constexpr size_type InlineCapacity = (15 / sizeof(T)) - 1;
    static constexpr std::uint8_t HeapFlag = 0x80;

    inline LSStringBase() noexcept
    {
        std::memset(storage_, 0, sizeof(storage_));
    }

    inline LSStringBase(T const* s)
        : LSStringBase(s, s ? Length(s) : 0)
    {}

    inline LSStringBase(T const* s, size_type len)
    {
        std::memset(storage_, 0, sizeof(storage_));
        assign(s, len);
    }

    explicit inline LSStringBase(std::basic_string_view<T> s)
        : LSStringBase(s.data(), s.size())
    {}

    explicit inline LSStringBase(std::basic_string<T> const& s)
        : LSStringBase(s.data(), s.size())
    {}

    inline LSStringBase(size_type count, T ch)
    {
        std::memset(storage_, 0, sizeof(storage_));
        resize(count, ch);
    }

    inline LSStringBase(LSStringBase const& o)
        : LSStringBase(o.data(), o.size())
    {}

    inline LSStringBase(LSStringBase&& o) noexcept
    {
        std::memcpy(storage_, o.storage_, sizeof(storage_));
        std::memset(o.storage_, 0, sizeof(o.storage_));
    }

    inline ~LSStringBase()
    {
        release();
    }

    inline LSStringBase& operator = (LSStringBase const& o)
    {
        if (this != &o) assign(o.data(), o.size());
        return *this;
    }

    inline LSStringBase& operator = (LSStringBase&& o) noexcept
    {
        if (this != &o) {
            release();
            std::memcpy(storage_, o.storage_, sizeof(storage_));
            std::memset(o.storage_, 0, sizeof(o.storage_));
        }
        return *this;
    }

    inline LSStringBase& operator = (T const* s)
    {
        assign(s, s ? Length(s) : 0);
        return *this;
    }

    inline LSStringBase& operator = (std::basic_string_view<T> s)
    {
        assign(s.data(), s.size());
        return *this;
    }

    inline bool is_heap() const noexcept
    {
        return (storage_[sizeof(storage_) - 1] & HeapFlag) != 0;
    }

    inline T const* data() const noexcept
    {
        return is_heap() ? heap_pointer() : (T const*)storage_;
    }

    inline T* data() noexcept
    {
        return is_heap() ? heap_pointer() : (T*)storage_;
    }

    inline T const* c_str() const noexcept
    {
        return data();
    }

    inline size_type size() const noexcept
    {
        if (!is_heap()) return storage_[sizeof(storage_) - 1];
        std::uint32_t n = 0;
        std::memcpy(&n, storage_ + sizeof(void*), sizeof(n));
        return n;
    }

    inline size_type length() const noexcept { return size(); }
    inline bool empty() const noexcept { return size() == 0; }

    inline size_type capacity() const noexcept
    {
        if (!is_heap()) return InlineCapacity;
        std::uint32_t n = 0;
        std::memcpy(&n, storage_ + sizeof(void*) + 4, sizeof(n));
        return n & 0x7fffffffu;
    }

    inline const_reference operator [] (size_type i) const { return data()[i]; }
    inline reference operator [] (size_type i) { return data()[i]; }

    inline const_iterator begin() const noexcept { return data(); }
    inline const_iterator end() const noexcept { return data() + size(); }
    inline iterator begin() noexcept { return data(); }
    inline iterator end() noexcept { return data() + size(); }

    inline std::reverse_iterator<const_iterator> rbegin() const noexcept
    {
        return std::reverse_iterator<const_iterator>(end());
    }

    inline std::reverse_iterator<const_iterator> rend() const noexcept
    {
        return std::reverse_iterator<const_iterator>(begin());
    }

    inline operator std::basic_string_view<T> () const noexcept
    {
        return std::basic_string_view<T>(data(), size());
    }

    inline void clear()
    {
        set_size(0);
        data()[0] = (T)0;
    }

    inline void assign(T const* s, size_type len)
    {
        reserve(len);
        if (len != 0 && s != nullptr) std::memcpy(data(), s, len * sizeof(T));
        set_size(len);
        data()[len] = (T)0;
    }

    inline void resize(size_type len, T ch = (T)0)
    {
        const size_type was = size();
        reserve(len);
        for (size_type i = was; i < len; ++i) data()[i] = ch;
        set_size(len);
        data()[len] = (T)0;
    }

    inline void reserve(size_type want)
    {
        if (want <= capacity()) return;

        // Grown to exactly what is asked for plus a terminator; these
        // strings are read far more often than they are built.
        auto* buffer = Allocate(want + 1);
        const size_type len = size();
        if (len != 0) std::memcpy(buffer, data(), len * sizeof(T));
        buffer[len] = (T)0;
        release();

        std::memset(storage_, 0, sizeof(storage_));
        std::memcpy(storage_, &buffer, sizeof(buffer));
        const auto capacity = (std::uint32_t)want | 0x80000000u;
        std::memcpy(storage_ + sizeof(void*) + 4, &capacity, sizeof(capacity));
        set_size(len);
    }

    inline void push_back(T ch)
    {
        const size_type len = size();
        reserve(len + 1);
        data()[len] = ch;
        set_size(len + 1);
        data()[len + 1] = (T)0;
    }

    inline LSStringBase& append(T const* s, size_type len)
    {
        const size_type was = size();
        reserve(was + len);
        if (len != 0) std::memcpy(data() + was, s, len * sizeof(T));
        set_size(was + len);
        data()[was + len] = (T)0;
        return *this;
    }

    inline LSStringBase& append(T const* s) { return append(s, Length(s)); }
    inline LSStringBase& append(LSStringBase const& s)
    {
        return append(s.data(), s.size());
    }

    inline LSStringBase& operator += (T ch) { push_back(ch); return *this; }
    inline LSStringBase& operator += (T const* s) { return append(s); }
    inline LSStringBase& operator += (LSStringBase const& s)
    {
        return append(s);
    }
    inline LSStringBase& operator += (std::basic_string_view<T> s)
    {
        return append(s.data(), s.size());
    }

    inline LSStringBase substr(size_type from, size_type len = npos) const
    {
        const size_type n = size();
        if (from > n) from = n;
        if (len > n - from) len = n - from;
        return LSStringBase(data() + from, len);
    }

    inline bool starts_with(std::basic_string_view<T> s) const
    {
        return view().substr(0, s.size()) == s;
    }

    inline bool ends_with(std::basic_string_view<T> s) const
    {
        return size() >= s.size() && view().substr(size() - s.size()) == s;
    }

    inline bool contains(std::basic_string_view<T> s) const
    {
        return view().find(s) != npos;
    }

    inline bool contains(T ch) const { return view().find(ch) != npos; }

    inline size_type find(T ch, size_type from = 0) const
    {
        return view().find(ch, from);
    }

    inline size_type find(T const* s, size_type from = 0) const
    {
        return view().find(s, from);
    }

    inline size_type find_first_of(T ch, size_type from = 0) const
    {
        return view().find_first_of(ch, from);
    }

    inline size_type find_last_of(T ch, size_type from = npos) const
    {
        return view().find_last_of(ch, from);
    }

    inline int compare(LSStringBase const& o) const
    {
        return view().compare(o.view());
    }

    inline bool operator == (LSStringBase const& o) const
    {
        return view() == o.view();
    }

    inline bool operator != (LSStringBase const& o) const
    {
        return !(*this == o);
    }

    inline bool operator < (LSStringBase const& o) const
    {
        return view() < o.view();
    }

    inline bool operator == (T const* s) const
    {
        return view() == std::basic_string_view<T>(s);
    }

    inline bool operator != (T const* s) const { return !(*this == s); }

private:
    inline std::basic_string_view<T> view() const noexcept
    {
        return std::basic_string_view<T>(data(), size());
    }

    inline T* heap_pointer() const noexcept
    {
        T* p = nullptr;
        std::memcpy(&p, storage_, sizeof(p));
        return p;
    }

    inline void set_size(size_type len)
    {
        if (!is_heap()) {
            storage_[sizeof(storage_) - 1] = (std::uint8_t)len;
        } else {
            const auto n = (std::uint32_t)len;
            std::memcpy(storage_ + sizeof(void*), &n, sizeof(n));
        }
    }

    inline void release()
    {
        if (is_heap()) Free(heap_pointer());
        std::memset(storage_, 0, sizeof(storage_));
    }

    static inline size_type Length(T const* s)
    {
        size_type n = 0;
        while (s[n] != (T)0) ++n;
        return n;
    }

    // Through the game's allocator, as the std::basic_string this replaces
    // did, so a string handed to the engine can still be freed by it.
    static inline T* Allocate(size_type count)
    {
        return static_cast<T*>(GameAllocRaw(count * sizeof(T)));
    }

    static inline void Free(T* p) { GameFree(p); }

    alignas(void*) std::uint8_t storage_[16];
};

static_assert(sizeof(LSStringBase<char>) == 16,
              "the native build's string is sixteen bytes");

// Concatenation, comparison and streaming, so the vendored code that builds
// messages and keys out of these keeps working unchanged.
template <class T>
inline LSStringBase<T> operator + (LSStringBase<T> const& a,
                                   LSStringBase<T> const& b)
{
    LSStringBase<T> out(a);
    out.append(b);
    return out;
}

template <class T>
inline LSStringBase<T> operator + (LSStringBase<T> const& a, T const* b)
{
    LSStringBase<T> out(a);
    out.append(b);
    return out;
}

template <class T>
inline LSStringBase<T> operator + (T const* a, LSStringBase<T> const& b)
{
    LSStringBase<T> out(a);
    out.append(b);
    return out;
}

template <class T>
inline LSStringBase<T> operator + (LSStringBase<T> const& a,
                                   std::basic_string_view<T> b)
{
    LSStringBase<T> out(a);
    out.append(b.data(), b.size());
    return out;
}

template <class T>
inline bool operator == (T const* a, LSStringBase<T> const& b)
{
    return b == a;
}

template <class T>
inline bool operator != (T const* a, LSStringBase<T> const& b)
{
    return !(b == a);
}

END_SE()

namespace std {

template <class T>
struct hash<bg3se::LSStringBase<T>>
{
    inline size_t operator () (bg3se::LSStringBase<T> const& s) const noexcept
    {
        return hash<basic_string_view<T>>{}(basic_string_view<T>(s));
    }
};

inline ostream& operator << (ostream& out, bg3se::LSStringBase<char> const& s)
{
    out.write(s.data(), (streamsize)s.size());
    return out;
}

}  // namespace std
