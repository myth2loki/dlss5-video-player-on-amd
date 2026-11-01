// Behavioural checks of the ported policy layer, on the things that a
// successful compile does not prove.
//
// The port compiles 50 policy headers that were written against Win32. What a
// compile cannot tell you is whether they still MEAN the same thing - and there
// are four places where they might not:
//
//   1. RECT's far edges. Exclusive on Windows; if the shim made them inclusive
//      every layout in the layer is off by one, and the toolbar would still
//      draw, just wrong.
//   2. COLORREF's byte order. 0x00BBGGRR on Windows. Reversed, every colour in
//      the player becomes its own complement - red where blue belongs.
//   3. MulDiv's rounding. Half away from zero, not truncating. Truncating
//      makes every DPI-scaled dimension a pixel short at some DPIs and not
//      others, which is the kind of bug that survives a screenshot review.
//   4. LONG's width against `%ld`. This is the one real difference the port
//      has to make a decision about: on Windows long is 32 bits, here it is
//      64. Round-tripping a saved window placement is what proves the decision
//      (see UiTypes.h) is coherent.

#include "PlatformCompat.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what)
{
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what);
    if (!condition) ++g_failures;
}

void CheckNear(long long actual, long long expected, const char* what)
{
    const bool ok = actual == expected;
    std::printf(ok ? "  ok    %s (%lld)\n" : "  FAIL  %s (got %lld, wanted %lld)\n", what, actual,
                expected);
    if (!ok) ++g_failures;
}

} // namespace

int main()
{
    std::printf("geometry\n");
    {
        // Right/bottom exclusive: a 10-wide rect ends at 10 and its width is 10.
        const RECT r{0, 0, 10, 10};
        CheckNear(r.Width(), 10, "RECT width is right-left");
        CheckNear(r.Height(), 10, "RECT height is bottom-top");

        // The far edge is NOT inside, which is what every hit test assumes.
        POINT inside{9, 9};
        POINT edge{10, 10};
        Check(PtInRect(r, inside), "the last pixel is inside");
        Check(!PtInRect(r, edge), "the far edge is outside");

        // OffsetRect moves both edges; a version that moved only one would
        // shrink the rect instead of sliding it.
        RECT moved = r;
        OffsetRect(&moved, 5, 3);
        CheckNear(moved.left, 5, "OffsetRect moves left");
        CheckNear(moved.right, 15, "OffsetRect moves right");
        CheckNear(moved.Width(), 10, "OffsetRect preserves width");
    }

    std::printf("\ncolo(u)r\n");
    {
        // The palette's own first entry, which the presenter clears the window
        // to. If this order is wrong the window is a different colour entirely.
        constexpr COLORREF window = RGB(18, 19, 21);
        CheckNear(GetRValue(window), 18, "GetRValue reads red");
        CheckNear(GetGValue(window), 19, "GetGValue reads green");
        CheckNear(GetBValue(window), 21, "GetBValue reads blue");

        // The Metal texel is BGRA little-endian, so red lands at bit 16. This
        // is the one conversion the port actually has to do.
        const uint32_t texel = ui::ToBgra(window);
        CheckNear((texel >> 16) & 0xff, 18, "ToBgra puts red at bit 16");
        CheckNear((texel >> 8) & 0xff, 19, "ToBgra puts green at bit 8");
        CheckNear(texel & 0xff, 21, "ToBgra puts blue at bit 0");
        CheckNear((texel >> 24) & 0xff, 0xff, "ToBgra is opaque");
    }

    std::printf("\nMulDiv rounding\n");
    {
        // 96 -> 144 DPI is the common Retina step; a truncating MulDiv would
        // give 14 instead of 15 for 10 dip and every gap would drift.
        CheckNear(MulDiv(10, 144, 96), 15, "10 dip at 144 dpi is 15 px");
        CheckNear(MulDiv(10, 96, 96), 10, "10 dip at 96 dpi is 10 px");
        // A half rounds away from zero, which is Win32's rule.
        CheckNear(MulDiv(3, 1, 2), 2, "3/2 rounds to 2");
        CheckNear(MulDiv(-3, 1, 2), -2, "-3/2 rounds to -2");
        CheckNear(MulDiv(1, 1, 3), 0, "1/3 rounds to 0");
        // The documented failure answer, which callers check for.
        CheckNear(MulDiv(1, 1, 0), -1, "a zero denominator gives -1");
    }

    std::printf("\nLONG and %%ld\n");
    {
        // The port's one deliberate width difference. LONG is `long` so that
        // `%ld` matches it; this asserts the pairing stays coherent, because
        // a change to either half breaks it silently.
        wchar_t text[64]{};
        const LONG left = -1920;
        swprintf_s(text, L"%ld,%ld", left, static_cast<LONG>(3840));
        const std::wstring formatted(text);
        Check(formatted == L"-1920,3840", "a LONG formats and reads back through %ld");

        // Round-tripping is the actual use: WindowPlacementPolicy writes this
        // string to an ini and parses it back.
        long a = 0, b = 0;
        const int assigned = swscanf_s(text, L"%ld,%ld", &a, &b);
        CheckNear(assigned, 2, "swscanf_s reports two fields");
        CheckNear(a, -1920, "the first value round-trips");
        CheckNear(b, 3840, "the second value round-trips");
    }

    std::printf("\nstring helpers\n");
    {
        wchar_t buffer[8]{};
        CheckNear(swprintf_s(buffer, L"%d", 42), 2, "swprintf_s returns the length written");
        Check(std::wstring(buffer) == L"42", "the value is in the buffer");

        // Truncation has to report -1 and still terminate, because a caller
        // that ignores the return still must not read past the buffer.
        //
        // %ls, not %s: in a wide format string %s means a NARROW string, so
        // passing a wchar_t* to it is a type error the compiler does not catch
        // in a varargs call. Every policy file already uses %ls; this test said
        // %s first and caught itself.
        wchar_t small[4]{};
        CheckNear(swprintf_s(small, L"%ls", L"abcdefgh"), -1, "truncation reports -1");
        Check(small[3] == L'\0', "a truncated buffer is still terminated");
    }

    std::printf("\nencoding\n");
    {
        // UTF-8 in, wide out, and back. The path round-trip is what the digest
        // memo relies on, so it has to be lossless for non-ASCII.
        const char* utf8 = "音乐/视频.mp4";
        wchar_t wide[64]{};
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 64);
        Check(wideLength > 0, "UTF-8 converts to wide");
        Check(std::wstring(wide) == L"音乐/视频.mp4", "the wide text is the same");

        char back[64]{};
        const int backLength = WideCharToMultiByte(CP_UTF8, 0, wide, -1, back, 64, nullptr, nullptr);
        Check(backLength > 0, "wide converts back to UTF-8");
        Check(std::string(back) == utf8, "the round-trip is lossless");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
