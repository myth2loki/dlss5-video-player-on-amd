#pragma once

// The Win32 vocabulary the player's policy layer is written in, for platforms
// that are not Windows.
//
// The 56 *Policy.h files are 11.7k lines of genuinely portable logic - layout
// arithmetic, timeline hit-testing, status formatting, audio clock recovery -
// but they spell their geometry with RECT, POINT, SIZE and COLORREF, and their
// integers with UINT and DWORD. That spelling is not portability; it is just
// the vocabulary the file was written in.
//
// There were two ways to carry that vocabulary across:
//
//   1. rewrite 11.7k lines to use new names, and keep two copies of the logic;
//   2. define the names once, here, and keep one copy.
//
// This is (2). It is deliberately a shim, not an abstraction: nothing in the
// policy layer is polymorphic over platforms, and pretending otherwise would
// add indirection to code that is only ever doing integer arithmetic on four
// edges of a rectangle.
//
// The semantics are matched to Windows exactly where it matters:
//
//   - RECT's right/bottom are EXCLUSIVE, so RECT{0,0,10,10} is ten pixels
//     wide and `right - left` is the width. Every layout constant in the
//     policy files assumes this, and copying the struct with inclusive bounds
//     would be off by one everywhere.
//   - COLORREF is 0x00BBGGRR, and RGB() builds it that way, so a colour
//     literal copied from UiResources.h means the same thing on both
//     platforms. The Metal presenter converts to BGRA texels, which is the
//     same byte order - see ui::ToBgra below.
//   - UINT is `unsigned int` and DWORD is `uint32_t`, not `unsigned long`:
//     on LP64 a DWORD that stayed 64-bit would silently change every hash and
//     every printf of one.
//
// What is NOT here: HWND, HANDLE, HRESULT, ID3D12*. Those are not vocabulary,
// they are platform. The two policy files that need them (AudioEndpointPolicy,
// RendererRecoveryPolicy) are not portable and stay out of this layer.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace ui {

// ---- integers -------------------------------------------------------------
//
// Windows spells its fixed-width integers in its own words. Keeping the names
// is what lets a policy file compile unchanged, and keeping the WIDTHS is what
// keeps it correct.

using UINT = unsigned int;
using DWORD = uint32_t;
using WORD = uint16_t;
using BYTE = uint8_t;
using BOOL = int;

// LONG is `long`, not int32_t, and the reason is worth stating because the
// obvious choice is wrong.
//
// On Windows, long and LONG are both 32 bits, so either spelling works there.
// On LP64 they are not the same thing at all: long is 64 bits and int is 32.
// The policy layer writes `%ld` against a LONG (WindowPlacementPolicy's saved
// rectangle) and reads it back with `%ld` too, and a varargs format has to
// match its argument's width exactly or the call is undefined - the reader
// would take four bytes from an eight-byte slot and the value after it would
// be garbage.
//
// So the width follows the format, not the Windows struct: LONG is long, and
// `%ld` is right by construction. Nothing in this layer is binary-compatible
// with the Windows build and nothing needs to be - a RECT here is layout
// arithmetic in memory, never a serialised structure. The one place a width
// matters on disk (the ini's "left,top,right,bottom") is text, and `%ld` writes
// text.
//
// The consequence to know about: `LONG_MIN` and `LONG_MAX` are then <climits>'
// own, and must NOT be redefined here. An earlier version of this file did
// define them as the limits of int32_t, and every use site silently took the
// 64-bit <climits> value instead - because a using-declaration cannot beat a
// macro-scope constant that <climits> already introduced. SliderPolicy's
// default argument `LONG_MIN / 2` then failed to narrow into a field.
using LONG = long;

// ---- geometry -------------------------------------------------------------

// Right/bottom exclusive, exactly as Windows defines it. Every call site in
// the policy layer depends on that: a toolbar of width `right - left` and a
// hit test of `x >= left && x < right` are both written against it.
struct RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;

    LONG Width() const { return right - left; }
    LONG Height() const { return bottom - top; }
    bool Empty() const { return right <= left || bottom <= top; }
};

struct POINT {
    LONG x = 0;
    LONG y = 0;
};

struct SIZE {
    LONG cx = 0;
    LONG cy = 0;
};

// ---- colour ---------------------------------------------------------------

// 0x00BBGGRR, the byte order Windows stores a COLORREF in, so RGB(r,g,b) below
// produces the same integer the Windows build produces for the same literal.
using COLORREF = uint32_t;

constexpr BYTE GetRValue(COLORREF colour) { return BYTE(colour & 0xff); }
constexpr BYTE GetGValue(COLORREF colour) { return BYTE((colour >> 8) & 0xff); }
constexpr BYTE GetBValue(COLORREF colour) { return BYTE((colour >> 16) & 0xff); }

constexpr COLORREF RGB(BYTE red, BYTE green, BYTE blue)
{
    return COLORREF(red) | (COLORREF(green) << 8) | (COLORREF(blue) << 16);
}

// The destination the Metal presenter actually wants: a BGRA8 texel, which is
// the same three bytes in the reverse order plus an opaque alpha. Kept here
// rather than in the presenter so that the one definition of "what a colour
// becomes on screen" lives beside the one definition of what a colour is.
constexpr uint32_t ToBgra(COLORREF colour)
{
    return uint32_t(GetRValue(colour)) << 16 |
           uint32_t(GetGValue(colour)) << 8 |
           uint32_t(GetBValue(colour)) |
           0xff000000u;
}

// ---- rectangle helpers ----------------------------------------------------
//
// Same contracts as the Win32 functions of the same names, including the
// in-place mutation, because the policy files call them for their side effects
// inside layout loops.

inline void SetRect(RECT& rect, LONG left, LONG top, LONG right, LONG bottom)
{
    rect = RECT{left, top, right, bottom};
}

inline void OffsetRect(RECT& rect, LONG dx, LONG dy)
{
    rect.left += dx;
    rect.right += dx;
    rect.top += dy;
    rect.bottom += dy;
}

// The pointer form. Win32 takes a RECT* and the policy layer calls it that way
// in layout loops, so both spellings have to exist.
inline void OffsetRect(RECT* rect, LONG dx, LONG dy)
{
    if (rect) OffsetRect(*rect, dx, dy);
}

// IntersectRect: false when the two do not overlap, and the destination is
// then zeroed - Win32's contract, which WindowPlacementPolicy tests against
// before deciding a saved placement is visible. Win32's signature is three
// pointers, which is the form every call site uses.
inline bool IntersectRect(RECT* destination, const RECT* a, const RECT* b)
{
    if (!destination) return false;
    if (!a || !b) { *destination = RECT{}; return false; }
    RECT result;
    result.left = std::max(a->left, b->left);
    result.top = std::max(a->top, b->top);
    result.right = std::min(a->right, b->right);
    result.bottom = std::min(a->bottom, b->bottom);
    if (result.right <= result.left || result.bottom <= result.top) {
        *destination = RECT{};
        return false;
    }
    *destination = result;
    return true;
}

inline bool UnionRect(RECT* destination, const RECT* a, const RECT* b)
{
    if (!destination) return false;
    if (!a && !b) { *destination = RECT{}; return false; }
    if (!a) { *destination = *b; return !b->Empty(); }
    if (!b) { *destination = *a; return !a->Empty(); }
    if (a->Empty()) { *destination = *b; return !b->Empty(); }
    if (b->Empty()) { *destination = *a; return !a->Empty(); }
    destination->left = std::min(a->left, b->left);
    destination->top = std::min(a->top, b->top);
    destination->right = std::max(a->right, b->right);
    destination->bottom = std::max(a->bottom, b->bottom);
    return true;
}

// A negative inset SHRINKS, which is Win32's rule and the one the call sites
// rely on; getting this backwards would make every panel grow instead.
inline void InflateRect(RECT& rect, LONG dx, LONG dy)
{
    rect.left -= dx;
    rect.right += dx;
    rect.top -= dy;
    rect.bottom += dy;
}

inline bool PtInRect(const RECT& rect, POINT point)
{
    // Exclusive on the far edges, matching RECT itself.
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

inline bool EqualRect(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

inline bool IsRectEmpty(const RECT& rect) { return rect.Empty(); }

inline void CopyRect(RECT& destination, const RECT& source) { destination = source; }

// ---- arithmetic -----------------------------------------------------------

// Windows' MulDiv rounds half away from zero rather than truncating, and the
// policy layer's DPI scaling is written against that: every dimension in the
// player comes through here, so a truncating version would make the whole UI
// a pixel short at some DPI and not others.
inline int MulDiv(int number, int numerator, int denominator)
{
    if (denominator == 0) return -1;   // Win32's documented "no answer"
    const int64_t product = int64_t(number) * int64_t(numerator);
    const int64_t half = denominator > 0 ? denominator / 2 : -((-denominator) / 2);
    return int(product >= 0 ? (product + half) / denominator : (product - half) / denominator);
}

// ---- wide-string helpers --------------------------------------------------
//
// The secure CRT's wide-string functions. MSVC has no plain swprintf that
// takes a count destination, so the policy files use these; libc++ has no
// equivalent of the _s forms at all, so they are written here rather than
// mapped.
//
// Both call forms in the policy layer take an ARRAY and no count -
// `swprintf_s(text, L"%lld", n)` - because MSVC has a template overload that
// deduces the size from the array type. That is the form implemented here: a
// template on the array extent, so a call written against Windows compiles
// against it unchanged and still cannot overrun the buffer.
//
// They return the number of characters written (never counting the
// terminator), or -1 on truncation - the MSVC contract the call sites test.

namespace detail {

// The actual formatting, with the count supplied by the template below.
inline int WidePrintF(wchar_t* buffer, size_t count, const wchar_t* format, va_list arguments)
{
    if (!buffer || count == 0) return -1;
    // vswprintf takes the count and does NOT guarantee a terminator when it
    // truncates, so one is forced afterwards either way.
    const int written = ::vswprintf(buffer, count, format, arguments);
    if (written < 0) {
        buffer[count - 1] = L'\0';
        return -1;
    }
    return written;
}

} // namespace detail

template <size_t N>
int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int written = detail::WidePrintF(buffer, N, format, arguments);
    va_end(arguments);
    return written;
}

// The counted form, for a call site that already has a pointer and a size.
inline int swprintf_s(wchar_t* buffer, size_t count, const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int written = detail::WidePrintF(buffer, count, format, arguments);
    va_end(arguments);
    return written;
}

// The scanning counterpart. Returns the number of fields assigned, as swscanf
// does, or -1 - and an empty string is 0 fields rather than an error, which is
// what WindowPlacementPolicy tests.
inline int swscanf_s(const wchar_t* source, const wchar_t* format, ...)
{
    if (!source || !format) return -1;
    va_list arguments;
    va_start(arguments, format);
    const int assigned = ::vswscanf(source, format, arguments);
    va_end(arguments);
    return assigned;
}

// ---- constants ------------------------------------------------------------

// GetSystemMetrics(SM_CXSCREEN)'s neighbour: the DPI a window is drawn at when
// nothing says otherwise. Every D() in the policy layer divides or multiplies
// by it.
constexpr UINT USER_DEFAULT_SCREEN_DPI = 96;

// The code page name the UTF-8 conversions pass. Nothing in this layer
// converts between encodings any more - the port reads UTF-8 throughout - but
// the constant appears in a signature that a policy file still declares.
constexpr UINT CP_UTF8 = 65001;

// WC_ERR_INVALID_CHARS: make MultiByteToWideChar reject malformed input rather
// than substituting. The player's digests are computed over converted paths, so
// a permissive conversion would make two different files hash the same.
constexpr DWORD WC_ERR_INVALID_CHARS = 0x00000080;
constexpr DWORD MB_ERR_INVALID_CHARS = 0x00000008;

// GetExitCodeProcess's "the process has not finished". AudioStderrPolicy reads
// an exit code and compares it against this to decide whether a helper died or
// is still running; the value itself is Windows' (259) but the decision it
// makes is portable - on POSIX the equivalent is WIFEXITED, and a helper that
// exited normally never reports this value.
constexpr DWORD STILL_ACTIVE = 259;

// MessageBox's answers. Only the two that carry a decision matter; a caller
// compares against IDYES, and IDNO versus IDCANCEL is a distinction that
// belongs to the dialog rather than to the policy reading its result. Whatever
// shows the box on this platform returns these values.
constexpr int IDOK = 1;
constexpr int IDCANCEL = 2;
constexpr int IDYES = 6;
constexpr int IDNO = 7;

// DWORD_MAX and UINT_MAX are NOT defined here, deliberately, and the reason is
// a bug this file had: <climits> and some system headers define UINT_MAX as a
// MACRO, and a macro beats any declaration - so `constexpr UINT UINT_MAX`
// expanded to `constexpr UINT 4294967295u` and failed to parse. Win32's own
// UINT_MAX comes from <limits.h> the same way, so the std ones are already
// present and already correct; nothing needs adding.

// ---- encoding -------------------------------------------------------------
//
// The player carries its strings as UTF-16 (std::wstring) because that is what
// the Windows API takes, and it converts at the edges with MultiByteToWideChar.
// macOS has no such function and no 2-byte wchar_t at all, so this is the one
// place where "the same vocabulary" cannot be "the same semantics": a wchar_t
// here is 32 bits and holds a code point, not a UTF-16 unit.
//
// The conversion is written out rather than delegated to mbsrtowcs/wcsrtombs.
// That was the first attempt and it was wrong for a reason worth recording:
// the mb* functions obey the C locale, and a GUI process starts in "C", where
// every byte above 0x7f is an error. A player launched from Finder has never
// called setlocale, so every non-ASCII path would fail to convert and the
// failure would appear only for users whose files have non-ASCII names - which,
// for a Chinese-language player, is most of them.
//
// These decode UTF-8 directly. They handle the full range including astral
// planes, replacing a malformed sequence with U+FFFD rather than failing, which
// is what Windows does without WC_ERR_INVALID_CHARS.

namespace detail {

constexpr wchar_t kReplacement = 0xFFFD;

// Reads one code point from `text` at `index`, advancing it. Returns false at
// the end. A malformed sequence consumes one byte and yields U+FFFD.
inline bool DecodeUtf8(const char* text, size_t size, size_t& index, uint32_t& code)
{
    if (index >= size) return false;
    const uint8_t first = uint8_t(text[index]);
    if (first < 0x80) {
        code = first;
        index += 1;
        return true;
    }

    int extra = 0;
    uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) { extra = 1; value = first & 0x1F; }
    else if ((first & 0xF0) == 0xE0) { extra = 2; value = first & 0x0F; }
    else if ((first & 0xF8) == 0xF0) { extra = 3; value = first & 0x07; }
    else { code = kReplacement; index += 1; return true; }

    if (index + size_t(extra) >= size) { code = kReplacement; index += 1; return true; }
    for (int offset = 1; offset <= extra; ++offset) {
        const uint8_t next = uint8_t(text[index + size_t(offset)]);
        if ((next & 0xC0) != 0x80) { code = kReplacement; index += 1; return true; }
        value = (value << 6) | (next & 0x3F);
    }
    index += size_t(extra) + 1;
    code = value;
    return true;
}

// Appends one code point as UTF-8.
inline void EncodeUtf8(uint32_t code, std::string& out)
{
    if (code < 0x80) {
        out += char(code);
    } else if (code < 0x800) {
        out += char(0xC0 | (code >> 6));
        out += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += char(0xE0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    } else {
        out += char(0xF0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3F));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
}

} // namespace detail

// MultiByteToWideChar's contract: a negative sourceLength means "to the
// terminator and include it", a null destination means "report the size", and
// the count returned includes the terminator only in the negative-length case -
// which is the detail the first implementation got wrong.
inline int MultiByteToWideChar(UINT codePage, DWORD /*flags*/, const char* source, int sourceLength,
                               wchar_t* destination, int destinationLength)
{
    (void)codePage;  // UTF-8 is the only code page this port has.
    if (!source) return 0;

    const bool terminated = sourceLength < 0;
    const size_t size = terminated ? std::strlen(source) : size_t(sourceLength);
    if (size == 0 && !terminated) return 0;

    size_t index = 0;
    int written = 0;
    while (index < size) {
        uint32_t code = 0;
        if (!detail::DecodeUtf8(source, size, index, code)) break;
        if (destination && written < destinationLength) destination[written] = wchar_t(code);
        ++written;
    }
    if (terminated) {
        // The terminator counts towards the returned length, as Win32 does when
        // sourceLength is -1.
        if (destination && written < destinationLength) destination[written] = L'\0';
        ++written;
    } else if (destination && written < destinationLength) {
        destination[written] = L'\0';
    }
    return written;
}

inline int WideCharToMultiByte(UINT codePage, DWORD /*flags*/, const wchar_t* source, int sourceLength,
                               char* destination, int destinationLength, const char* /*defaultChar*/,
                               BOOL* /*usedDefault*/)
{
    (void)codePage;
    if (!source) return 0;

    const bool terminated = sourceLength < 0;
    const size_t size = terminated ? std::wcslen(source) : size_t(sourceLength);
    if (size == 0 && !terminated) return 0;

    std::string encoded;
    encoded.reserve(size * 3);
    for (size_t index = 0; index < size; ++index) {
        detail::EncodeUtf8(uint32_t(source[index]), encoded);
    }
    if (terminated) encoded += '\0';

    const int needed = int(encoded.size());
    if (!destination) return needed;
    const int writable = std::min(needed, destinationLength);
    if (writable > 0) std::memcpy(destination, encoded.data(), size_t(writable));
    return writable;
}

} // namespace ui

// The names themselves, unqualified, because the policy files use them
// unqualified and that is the entire point of this header.
using ui::BOOL;
using ui::BYTE;
using ui::COLORREF;
using ui::CP_UTF8;
using ui::DWORD;
using ui::IDCANCEL;
using ui::IDNO;
using ui::IDOK;
using ui::IDYES;
using ui::LONG;
using ui::MB_ERR_INVALID_CHARS;
using ui::POINT;
using ui::RECT;
using ui::SIZE;
using ui::STILL_ACTIVE;
using ui::UINT;
using ui::USER_DEFAULT_SCREEN_DPI;
using ui::WC_ERR_INVALID_CHARS;
using ui::WORD;

using ui::CopyRect;
using ui::EqualRect;
using ui::GetBValue;
using ui::GetGValue;
using ui::GetRValue;
using ui::InflateRect;
using ui::IntersectRect;
using ui::IsRectEmpty;
using ui::MulDiv;
using ui::MultiByteToWideChar;
using ui::OffsetRect;
using ui::PtInRect;
using ui::RGB;
using ui::SetRect;
using ui::swprintf_s;
using ui::swscanf_s;
using ui::UnionRect;
using ui::WideCharToMultiByte;
