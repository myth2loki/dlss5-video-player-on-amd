// Standalone assertions for CastHttpServer.h pure policy functions.
#include "CastHttpServer.h"

#include <cstdio>

using namespace cast_http_server;

namespace {
int g_pass = 0;
int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)
} // namespace

int main()
{
    // Strict allowlist: only the two names HLS emits.
    CHECK(IsAllowedName("live.m3u8"), "m3u8 allowed");
    CHECK(IsAllowedName("seg-000001.ts"), "segment allowed");
    CHECK(IsAllowedName("seg-123456789.ts"), "long segment number allowed");
    CHECK(!IsAllowedName("seg-.ts"), "empty segment number rejected");
    CHECK(!IsAllowedName("seg-12a.ts"), "non-digit segment rejected");
    CHECK(!IsAllowedName("seg-12.ts.bak"), "suffix must be exactly .ts");
    CHECK(!IsAllowedName("../live.m3u8"), "traversal rejected");
    CHECK(!IsAllowedName("..\\live.m3u8"), "backslash traversal rejected");
    CHECK(!IsAllowedName("/live.m3u8"), "absolute path rejected");
    CHECK(!IsAllowedName("live.m3u8?x=1"), "querystring rejected");
    CHECK(!IsAllowedName("secret.txt"), "arbitrary file rejected");

    // Range parsing.
    auto r1 = ParseRange("bytes=0-99", 1000);
    CHECK(r1 && r1->first == 0 && r1->last == 99, "closed range");
    auto r2 = ParseRange("bytes=100-", 1000);
    CHECK(r2 && r2->first == 100 && r2->last == 999, "open range");
    auto r3 = ParseRange("bytes=-50", 1000);
    CHECK(r3 && r3->first == 950 && r3->last == 999, "suffix range");
    auto r4 = ParseRange("bytes=-5000", 1000);
    CHECK(r4 && r4->first == 0 && r4->last == 999, "suffix larger than file clamps");
    auto r5 = ParseRange("bytes=0-99999", 1000);
    CHECK(r5 && r5->last == 999, "range end past EOF clamps");
    CHECK(!ParseRange("bytes=1000-", 1000), "range start at EOF is unsatisfiable");
    CHECK(!ParseRange("bytes=20-10", 1000), "reversed range rejected");
    CHECK(!ParseRange("bytes=abc-def", 1000), "garbage range rejected");
    CHECK(!ParseRange("items=0-1", 1000), "wrong unit rejected");
    CHECK(!ParseRange("bytes=999999999999999999999999-", 1000), "overflow range rejected");

    std::printf("CastHttpServer tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
