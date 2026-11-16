// Headless checks of the audio pipeline and, in particular, of the clock.
//
// The clock is the reason this exists. "Sound plays" is not the requirement -
// the player's whole picture pipeline is slaved to the audio position, so what
// has to be true is that the position advances at real-time rate, that it is
// monotonic, and that a pause freezes it rather than losing it.
//
// This needs an output device. On a machine without one AudioPlayer::Start
// returns false by design and the checks below say so instead of failing;
// CI on a headless runner would take that branch.

#include "AudioPlayer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int g_failures = 0;
int g_skipped = 0;

void Check(bool condition, const std::string& what)
{
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++g_failures;
}

void Skip(const std::string& what)
{
    std::printf("  skip  %s\n", what.c_str());
    ++g_skipped;
}

double WallSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video-with-audio>\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];

    std::printf("probe audio in %s\n", path.c_str());
    AudioTrackInfo track;
    Check(AudioPlayer::Probe(path, track), "ffprobe reports an audio stream");
    std::printf("  codec=%s  %d Hz  %d ch\n", track.codec.c_str(), track.sampleRate,
                track.channels);

    // The Audio menu is built from ALL audio streams, not just the first. A
    // probe that reported one track would make the menu a single dead row.
    std::vector<AudioTrackInfo> tracks;
    Check(AudioPlayer::ProbeTracks(path, tracks), "ffprobe enumerates every audio stream");
    Check(!tracks.empty(), "at least one audio track is listed");
    Check(tracks.front().streamIndex == 0, "the first audio stream is 0:a:0");
    Check(track.streamIndex >= 0 && track.streamIndex < int(tracks.size()),
          "the default track is one of the enumerated tracks");

    AudioPlayer player;
    const auto started = player.Start(path, 0.0, AudioStartState::Playing);
    if (!started) {
        Skip("no usable output device here; the clock and the rest are untested");
        std::printf("\nskipped (%d)\n", g_skipped);
        return 0;
    }
    Check(player.Active(), "the sink opened");
    Check(player.Tracks().size() == tracks.size(),
          "Start caches the same track list the menu was built from");
    Check(player.TrackIndex() >= 0 && player.TrackIndex() < int(player.Tracks().size()),
          "Start records which stream is actually playing");

    // Audio is not instantaneous: the child has to be spawned and its first
    // frames decoded before the device has anything to consume. Measure that
    // rather than assuming it, because it is the latency a viewer hears as the
    // gap between the picture appearing and the sound starting.
    const double spawnAt = WallSeconds();
    double first = -1.0;
    while (WallSeconds() - spawnAt < 5.0) {
        first = player.PositionSeconds();
        if (first >= 0.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const double startup = WallSeconds() - spawnAt;
    std::printf("  first audio position after %.0f ms\n", startup * 1000.0);
    Check(first >= 0.0, "the clock answers once audio is flowing");
    Check(startup < 2.0, "audio starts within two seconds");

    // Only now measure the rate, so the -1 sentinel cannot pollute the delta.
    const double wallFirst = WallSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const double second = player.PositionSeconds();
    const double wallSecond = WallSeconds();
    const double audioDelta = second - first;
    const double wallDelta = wallSecond - wallFirst;
    std::printf("  over %.3fs of wall time the clock moved %.3fs\n", wallDelta, audioDelta);
    // 15% either way: the device period quantizes the reading, and the first
    // buffers were still filling when `first` was taken.
    Check(audioDelta > wallDelta * 0.85 && audioDelta < wallDelta * 1.15,
          "the clock advances at real-time rate");
    Check(second > first, "the clock is monotonic while playing");

    // A pause must freeze the clock without losing the position, and a resume
    // must carry on from there. This is the contract the presentation gate
    // depends on: a frozen clock shows a still frame, a reset one rewinds.
    const double beforePause = player.PositionSeconds();
    player.Pause(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const double whilePaused = player.PositionSeconds();
    std::printf("  paused: %.3fs -> %.3fs\n", beforePause, whilePaused);
    // audio_clock::Present carries a stalled clock forward on purpose when the
    // stream stops answering; while paused, Usable() is told playing=false and
    // the position is allowed to stand still. A small move is the queue
    // draining, which is deliberate.
    Check(std::abs(whilePaused - beforePause) < 0.5, "the clock does not run away while paused");

    player.Pause(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double afterResume = player.PositionSeconds();
    std::printf("  resumed: %.3fs -> %.3fs\n", whilePaused, afterResume);
    Check(afterResume > whilePaused - 0.2, "the clock resumes from where it was");

    // A seek positions the clock at the target, which is what makes the
    // picture jump with it.
    const double target = 1.5;
    player.Seek(target);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double afterSeek = player.PositionSeconds();
    std::printf("  sought to %.3fs, clock reads %.3fs\n", target, afterSeek);
    Check(afterSeek >= target - 0.1 && afterSeek < target + 0.8,
          "after a seek the clock starts at the target");

    player.Stop();
    Check(!player.Active(), "stop closes the sink");
    Check(player.PositionSeconds() < 0.0, "a stopped player reports no clock, so the caller falls back");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
