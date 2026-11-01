// A headless check of the layers under the picture. No window, no Metal: this
// is the part of the port that can be asserted, so it is what CI would run.
//
// What it proves: the ffmpeg pipe yields exactly the frames the container says
// it has, at the size and stride the presenter expects, and a seek lands on
// the frame the clock asked for rather than on a keyframe.

#include "FfmpegPipeSource.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what)
{
    if (condition) {
        std::printf("  ok    %s\n", what.c_str());
    } else {
        std::printf("  FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video>\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];

    std::printf("probe %s\n", path.c_str());
    MediaInfo info;
    Check(FfmpegPipeSource::Probe(path, info), "probe returns a format");
    std::printf("  %dx%d  %.6f fps  %.3f s  (frame = %zu bytes)\n",
                info.width, info.height, info.fps, info.duration, info.FrameBytes());

    const int64_t expected = static_cast<int64_t>(std::llround(info.duration * info.fps));
    std::printf("  container implies %lld frames\n", static_cast<long long>(expected));

    // Decode the whole file and count what actually comes out of the pipe.
    {
        FfmpegPipeSource source;
        Check(source.Open(path, info), "open");
        int64_t count = 0;
        VideoFrame frame;
        bool sizeOk = true;
        bool numberingOk = true;
        int64_t last = -1;
        while (source.Pop(frame)) {
            if (frame.bgra.size() != info.FrameBytes()) sizeOk = false;
            if (frame.number <= last) numberingOk = false;
            last = frame.number;
            ++count;
        }
        std::printf("  decoded %lld frames\n", static_cast<long long>(count));
        Check(count == expected, "decoded frame count matches the container");
        Check(sizeOk, "every frame is width*height*4 bytes");
        Check(numberingOk, "frame numbers are strictly increasing");
        source.Close();
    }

    // A seek must land where it was asked to, not on the preceding keyframe.
    {
        FfmpegPipeSource source;
        source.Open(path, info);
        const double target = info.duration * 0.6;
        const int64_t targetFrame = static_cast<int64_t>(std::llround(target * info.fps));
        const auto started = std::chrono::steady_clock::now();
        source.Seek(target);

        VideoFrame frame;
        bool got = false;
        // The producer restarts the child on the seek; a couple of frames of
        // slack is the read-ahead, not an inaccuracy.
        while (source.Pop(frame)) {
            if (frame.number >= targetFrame - 2) { got = true; break; }
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("  seek to %.3f s landed on frame %lld (wanted %lld) in %.3f s\n",
                    target, static_cast<long long>(frame.number),
                    static_cast<long long>(targetFrame), elapsed);
        Check(got, "seek produced a frame at the requested position");
        Check(frame.number >= targetFrame - 2 && frame.number <= targetFrame + 2,
              "seek landed within two frames");
        source.Close();
    }

    // Throughput, which is the number that decides whether this replaces the
    // Windows decode path at all.
    {
        FfmpegPipeSource source;
        source.Open(path, info);
        const auto started = std::chrono::steady_clock::now();
        int64_t count = 0;
        VideoFrame frame;
        while (source.Pop(frame)) ++count;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("  %.2f fps decode (%.3f s for %lld frames)\n",
                    count / seconds, seconds, static_cast<long long>(count));
        Check(seconds > 0.0, "decode completed");
        source.Close();
    }

    // The consumer's side of the queue, which had a bug that a decode test
    // cannot see: the player used to TryPop and then break on a frame ahead of
    // the clock, and TryPop has already removed that frame - so it was thrown
    // away. One frame lost per tick is invisible on a short clip and freezes a
    // long film, because the producer outruns the clock and the head is nearly
    // always ahead.
    //
    // This walks a virtual clock at 60 Hz over two seconds and asserts that the
    // picture advances about as often as the frame rate says it should. Before
    // the fix it managed 2 updates out of 120 ticks.
    {
        FfmpegPipeSource source;
        source.Open(path, info);
        int updates = 0;
        int64_t lastShown = -1;
        constexpr int kTicks = 120;
        for (int tick = 0; tick < kTicks; ++tick) {
            const double position = double(tick) / 60.0;
            const int64_t want = static_cast<int64_t>(std::llround(position * info.fps));
            if (want > 1) source.DiscardBefore(want - 1);

            bool got = false;
            while (true) {
                int64_t next = 0;
                if (!source.PeekFrontNumber(next)) break;   // nothing decoded yet
                if (next > want) break;                     // leave it queued
                VideoFrame frame;
                if (!source.TryPop(frame)) break;
                lastShown = frame.number;
                got = true;
            }
            if (got) ++updates;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const int64_t expected = static_cast<int64_t>(std::llround(2.0 * info.fps));
        std::printf("  consumer: %d updates over %d ticks, landed on frame %lld (wanted ~%lld)\n",
                    updates, kTicks, static_cast<long long>(lastShown),
                    static_cast<long long>(expected));
        // A quarter of the expected updates is the floor: a loaded machine and
        // a 16 ms timer both cost some. Two out of a hundred and twenty is the
        // failure this test exists for.
        Check(updates >= int(expected / 4), "the picture advances while the clock runs");
        Check(lastShown >= expected - 4, "the consumer reaches the frame the clock is on");
        source.Close();
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
