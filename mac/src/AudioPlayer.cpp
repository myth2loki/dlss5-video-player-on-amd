#include "AudioPlayer.h"

#include "AudioTrackPolicy.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

extern char** environ;

namespace {

constexpr const char* kFfmpeg = "ffmpeg";
constexpr const char* kFfprobe = "ffprobe";

// Read granularity. The pipe delivers whatever it has, and a read of this size
// is a whole number of 20 ms buffers at 48 kHz stereo f32 (7680 bytes each),
// so the common case fills exactly one buffer.
constexpr size_t kReadBytes = 7680;

double WallSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string RunCapture(const std::string& command)
{
    std::string out;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return out;
    char buffer[4096];
    size_t got = 0;
    while ((got = ::fread(buffer, 1, sizeof(buffer), pipe)) > 0) out.append(buffer, got);
    ::pclose(pipe);
    return out;
}

std::string Quote(const std::string& value)
{
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string FormatDouble(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

// The ported Windows rule for which track to start on, applied to the probed
// list rather than to a second ffprobe call.
size_t DefaultAudioIndex(const std::vector<AudioTrackInfo>& tracks)
{
    std::vector<audio_track::Track> policy;
    policy.reserve(tracks.size());
    for (const AudioTrackInfo& info : tracks) {
        audio_track::Track track;
        track.audioIndex = info.streamIndex;
        track.language = info.language;
        track.title = info.title;
        track.codec = info.codec;
        track.channels = info.channels;
        track.sampleRate = info.sampleRate;
        track.startSeconds = info.startSeconds;
        track.isDefault = info.isDefault;
        track.comment = info.comment;
        track.visualImpaired = info.visualImpaired;
        track.descriptions = info.descriptions;
        track.hearingImpaired = info.hearingImpaired;
        policy.push_back(std::move(track));
    }
    const size_t chosen = audio_track::SelectDefault(policy);
    return chosen == audio_track::kNoTrack ? 0 : chosen;
}

} // namespace

struct AudioPlayer::ReaderState {
    // The child and its pipe. Owned by the reader thread for its whole life.
    int pipe = -1;
    int pid = -1;

    std::unique_ptr<CoreAudioSink> sink;
    uint32_t sampleRate = 0;

    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    // Set once the device has consumed anything. Until then the clock has no
    // business answering: a position before the first sample is heard is a
    // position nothing has been rendered at.
    std::atomic<bool> hasAudioData{false};
    std::atomic<bool> failed{false};

    // Latched when the default output device changed under a running stream.
    // The owner restarts the whole pipeline, because the new device's format
    // may differ and ffmpeg has to be told.
    std::atomic<bool> deviceLost{false};

    ~ReaderState()
    {
        if (pipe >= 0) ::close(pipe);
        if (pid > 0) {
            ::kill(pid_t(pid), SIGKILL);
            int status = 0;
            ::waitpid(pid_t(pid), &status, 0);
        }
    }
};

AudioPlayer::~AudioPlayer() { Stop(); }

bool AudioPlayer::ProbeTracks(const std::string& path, std::vector<AudioTrackInfo>& out)
{
    // One ffprobe call, one line per field, one block per audio stream. The
    // disposition and tag keys are what AudioTrackPolicy needs to avoid opening
    // a director's commentary just because the ripper listed it first.
    std::ostringstream command;
    command << kFfprobe << " -v error -select_streams a"
            << " -show_entries stream=index,codec_name,channels,sample_rate,start_time"
            << ":stream_tags=language,title"
            << ":stream_disposition=default,comment,visual_impaired,descriptions,hearing_impaired"
            << " -of default=noprint_wrappers=1 " << Quote(path);
    const std::string text = RunCapture(command.str());
    if (text.empty()) return false;

    std::vector<AudioTrackInfo> tracks;
    AudioTrackInfo current;
    bool inBlock = false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "index") {
            // A second index starts a new stream block. ffprobe reports the
            // GLOBAL stream index here; `-map 0:a:N` counts only audio streams,
            // so the position in this vector is the index that matters.
            if (inBlock) tracks.push_back(current);
            current = AudioTrackInfo{};
            current.streamIndex = int(tracks.size());
            inBlock = true;
            continue;
        }
        if (!inBlock) continue;
        if (key == "codec_name") current.codec = value;
        else if (key == "channels") current.channels = std::atoi(value.c_str());
        else if (key == "sample_rate") current.sampleRate = std::atoi(value.c_str());
        else if (key == "TAG:language") current.language = value;
        else if (key == "TAG:title") current.title = value;
        else if (key == "DISPOSITION:default") current.isDefault = value == "1";
        else if (key == "DISPOSITION:comment") current.comment = value == "1";
        else if (key == "DISPOSITION:visual_impaired") current.visualImpaired = value == "1";
        else if (key == "DISPOSITION:descriptions") current.descriptions = value == "1";
        else if (key == "DISPOSITION:hearing_impaired") current.hearingImpaired = value == "1";
        else if (key == "start_time") {
            char* end = nullptr;
            const double seconds = std::strtod(value.c_str(), &end);
            if (end != value.c_str() && std::isfinite(seconds)) current.startSeconds = seconds;
        }
    }
    if (inBlock) tracks.push_back(current);
    if (tracks.empty()) return false;
    out = std::move(tracks);
    return true;
}

bool AudioPlayer::Probe(const std::string& path, AudioTrackInfo& out)
{
    std::vector<AudioTrackInfo> tracks;
    if (!ProbeTracks(path, tracks)) return false;

    // Ordinary tracks first, the container's default among them; a commentary
    // wins only when there is nothing else.
    out = tracks[DefaultAudioIndex(tracks)];
    return true;
}

bool AudioPlayer::Start(const std::string& videoPath, double seekSeconds, AudioStartState state,
                         int audioIndex)
{
    Stop();

    m_path = videoPath;
    // Probing is a child process and a seek respawns this one; the container
    // has not changed, so the answer is cached per path.
    if (m_probedPath != videoPath) {
        if (!ProbeTracks(videoPath, m_tracks)) m_tracks.clear();
        m_probedPath = videoPath;
    }
    if (m_tracks.empty()) {
        // No audio track. Not a failure: a video-only file plays silently and
        // the caller falls back to its own clock.
        m_track = {};
        m_audioIndex = -1;
        return false;
    }

    // A caller that did not name a track gets the same default Probe() picks.
    if (audioIndex < 0 || audioIndex >= int(m_tracks.size()))
        audioIndex = int(DefaultAudioIndex(m_tracks));
    m_audioIndex = std::clamp(audioIndex, 0, int(m_tracks.size()) - 1);
    AudioTrackInfo track = m_tracks[size_t(m_audioIndex)];
    m_track = track;

    auto reader = std::make_shared<ReaderState>();
    reader->paused = (state == AudioStartState::Paused);
    m_seekBaseSeconds = seekSeconds;

    {
        std::scoped_lock lock(m_clockMutex);
        audio_clock::Reset(m_clock);
        audio_clock::Reset(m_continuity);
        m_clockStalled = false;
    }

    {
        std::scoped_lock lock(m_readerMutex);
        m_reader = reader;
    }

    m_thread = std::thread([this, reader, seekSeconds, state, path = videoPath, track = m_track] {
        ReaderLoop(reader, seekSeconds, state == AudioStartState::Paused, path, track);
    });

    // The thread may have failed on its first pass (no device, no ffmpeg);
    // reporting that as a refused start is what lets the caller stay on its
    // own clock instead of waiting for audio that is never coming.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reader->failed.load()) return false;
        if (reader->sink && reader->sink->IsOpen()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Slow start is not failure: a device that took longer than 500 ms is
    // still opening, and the reader thread will carry on.
    return true;
}

void AudioPlayer::ReaderLoop(std::shared_ptr<ReaderState> state, double seekSeconds, bool startPaused,
                             std::string path, AudioTrackInfo track)
{
    // Everything this thread needs arrives by value. It used to read m_path and
    // m_track, which Seek() rewrites on the calling thread while this one was
    // using them - a real race, found by reading rather than by crashing.
    (void)startPaused;

    // The format ffmpeg is told to produce. Asking for the source's own rate
    // and channel count means neither side resamples; 48 kHz stereo f32 is the
    // fallback, which is also what WasapiRenderer uses when the device cannot
    // be opened.
    CoreAudioSink::Format format;
    format.sampleRate = 48000;
    format.channels = 2;
    format.bitsPerSample = 32;
    format.isFloat = true;
    if (track.sampleRate > 0) format.sampleRate = uint32_t(track.sampleRate);
    if (track.channels > 0) format.channels = uint16_t(std::min(track.channels, 8));

    state->sink = std::make_unique<CoreAudioSink>();
    const auto opened = state->sink->Open(format);
    if (opened != CoreAudioSink::Status::Ok) {
        // No usable output device. The caller keeps its own clock and the film
        // plays silently - the same outcome the Windows player produces when
        // audio_endpoint finds nothing.
        std::fprintf(stderr, "audio: no output device; playing silently\n");
        state->failed = true;
        return;
    }
    state->sampleRate = format.sampleRate;
    state->sink->SetVolume(m_volume.load());

    // ffmpeg, as a child, writing raw frames to a pipe. The Windows arguments
    // carry over exactly: -map picks the stream, -vn/-sn/-dn drop the rest,
    // and the codec names the raw format the sink was just opened at.
    std::vector<std::string> args;
    args.push_back("-nostdin");
    args.push_back("-hide_banner");
    args.push_back("-v");
    args.push_back("error");
    if (seekSeconds > 0.0) {
        args.push_back("-ss");
        args.push_back(FormatDouble(seekSeconds));
    }
    args.push_back("-i");
    args.push_back(path);
    args.push_back("-map");
    // The selected audio stream. `?` keeps a video-only file from being an
    // error here, though Start() has already answered that case.
    args.push_back(std::string("0:a:") + std::to_string(std::max(0, track.streamIndex)) + "?");
    args.push_back("-vn");
    args.push_back("-sn");
    args.push_back("-dn");
    args.push_back("-ac");
    args.push_back(std::to_string(format.channels));
    args.push_back("-ar");
    args.push_back(std::to_string(format.sampleRate));
    args.push_back("-c:a");
    args.push_back(format.isFloat ? "pcm_f32le" : "pcm_s16le");
    args.push_back("-f");
    args.push_back(format.isFloat ? "f32le" : "s16le");
    args.push_back("pipe:1");

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(kFfmpeg));
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) {
        state->failed = true;
        return;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, kFfmpeg, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        state->failed = true;
        return;
    }
    state->pid = int(pid);
    state->pipe = fds[0];

    std::vector<uint8_t> buffer(kReadBytes);
    CoreAudioSink::AbortFlags abort;
    abort.stop = &state->stop;
    abort.paused = &state->paused;

    while (!state->stop.load()) {
        // Paused: nothing is read, which back-pressures ffmpeg through the pipe
        // instead of letting it race ahead. The queued frames have already been
        // faded out by FadeOutAndStop, so this is silence by omission rather
        // than by throwing data away. Note the order - Write is told about the
        // pause too, so a chunk already in flight does not restart the queue
        // the fade just stopped.
        if (state->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (state->deviceLost.load()) {
            // The owner restarts the pipeline on the new endpoint.
            break;
        }

        const ssize_t got = ::read(state->pipe, buffer.data(), buffer.size());
        if (got > 0) {
            const auto written = state->sink->Write(buffer.data(), size_t(got), abort);
            if (written == CoreAudioSink::WriteResult::Aborted) {
                // Stop or pause: neither is a failure, and neither consumed
                // this chunk, so loop rather than treating it as EOF.
                continue;
            }
            if (written == CoreAudioSink::WriteResult::Failed) {
                state->failed = true;
                break;
            }
            state->hasAudioData = true;
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;   // EOF or a dead child
    }

    state->sink->Close();
}

bool AudioPlayer::Seek(double seconds)
{
    const bool wasPaused = Paused();
    const float volume = m_volume.load();
    if (!Start(m_path, seconds,
               wasPaused ? AudioStartState::Paused : AudioStartState::Playing,
               m_audioIndex))
        return false;
    SetVolume(volume);
    return true;
}

void AudioPlayer::Pause(bool paused)
{
    std::shared_ptr<ReaderState> state;
    {
        std::scoped_lock lock(m_readerMutex);
        state = m_reader;
    }
    if (!state) return;

    {
        std::scoped_lock lock(m_clockMutex);
        audio_clock::PauseChanged(m_clock, m_continuity, WallSeconds());
    }
    state->paused = paused;
    if (!state->sink || !state->sink->IsOpen()) return;
    // A writer blocked on a full queue has to see the new flag; the queue's
    // callback is the only other thing that wakes it, and it is about to stop.
    state->sink->Wake();

    // Pausing ramps to silence and lets the queue drain before the device
    // stops, so the endpoint is never cut mid-waveform. Nothing is discarded:
    // the position ends at the end of what was queued rather than in the
    // middle of it, and resuming opens with a matching ramp.
    if (paused) state->sink->FadeOutAndStop();
    else state->sink->Start();
}

void AudioPlayer::SetVolume(float volume01)
{
    m_volume = std::clamp(volume01, 0.0f, 1.0f);
    std::shared_ptr<ReaderState> state;
    {
        std::scoped_lock lock(m_readerMutex);
        state = m_reader;
    }
    if (state && state->sink) state->sink->SetVolume(m_volume.load());
}

void AudioPlayer::Stop()
{
    std::shared_ptr<ReaderState> state;
    {
        std::scoped_lock lock(m_readerMutex);
        state = m_reader;
        m_reader.reset();
    }
    if (state) state->stop = true;
    // Join before anything else, and make sure a writer blocked on a full
    // queue can see the flag: without the wake this join waits for a device
    // that may never ask for another buffer.
    if (state && state->sink) state->sink->Wake();
    if (m_thread.joinable()) m_thread.join();
    if (state && state->sink) state->sink->Close();
    state.reset();

    std::scoped_lock lock(m_clockMutex);
    audio_clock::Reset(m_clock);
    audio_clock::Reset(m_continuity);
    m_clockStalled = false;
    m_lastKnownPosition = 0.0;
}

bool AudioPlayer::Active() const
{
    std::scoped_lock lock(m_readerMutex);
    return m_reader && m_reader->sink && m_reader->sink->IsOpen();
}

bool AudioPlayer::Paused() const
{
    std::scoped_lock lock(m_readerMutex);
    return m_reader && m_reader->paused.load();
}

double AudioPlayer::PositionSeconds() const
{
    std::shared_ptr<ReaderState> state;
    {
        std::scoped_lock lock(m_readerMutex);
        state = m_reader;
    }
    if (!state || !state->sink || !state->sampleRate || !state->hasAudioData.load()) return -1.0;

    uint64_t played = 0;
    if (!state->sink->PlayedFrames(played)) return -1.0;
    const double position = m_seekBaseSeconds.load() + double(played) / double(state->sampleRate);

    // hasAudioData is set once and cleared only by Stop(), so a reader thread
    // that has ended - pipe EOF, a dead ffmpeg child - leaves the queued frames
    // to drain and this position frozen. Returning it anyway would stop video
    // for the rest of the file, because the presentation gate holds every frame
    // until the clock reaches its due time. A stalled clock is carried forward
    // at wall-clock rate instead, and slewed back to the audio when it moves
    // again; see audio_clock::Present.
    const double now = WallSeconds();
    std::scoped_lock lock(m_clockMutex);
    const bool playing = !state->paused.load();
    const bool usable = audio_clock::Usable(m_clock, position, now, playing);
    const double presented = audio_clock::Present(m_continuity, position, usable, now, playing);
    m_lastKnownPosition.store(presented);

    if (usable && m_clockStalled) {
        m_clockStalled = false;
        std::fprintf(stderr, "audio: clock advancing again at %.3fs; slewing the master clock back\n",
                     position);
    } else if (!usable && !m_clockStalled) {
        m_clockStalled = true;
        std::fprintf(stderr,
                     "audio: clock frozen at %.3fs for over %.1fs; carrying it forward on the steady "
                     "clock. The helper has most likely ended, the stream has underrun, or the output "
                     "device has gone away.\n",
                     position, audio_clock::kStallSeconds);
    }
    return presented;
}

bool AudioPlayer::ServiceDeviceChanges(double playerPositionSeconds, bool playerPlaying)
{
    std::shared_ptr<ReaderState> state;
    {
        std::scoped_lock lock(m_readerMutex);
        state = m_reader;
    }
    if (!state || !state->sink) return false;

    // The default-device listener is installed by the macOS layer (main.mm),
    // which latches deviceLost on the reader; this is the tick that acts on it.
    // Keeping the decision here rather than in the callback is the same split
    // AudioEndpointPolicy makes on Windows, and for the same reason: it can be
    // argued with in a test rather than by unplugging things.
    if (!state->deviceLost.exchange(false)) return false;

    const double resumeAt = playerPositionSeconds >= 0.0
        ? playerPositionSeconds
        : m_lastKnownPosition.load();
    std::fprintf(stderr, "audio: output device changed; restarting at %.3fs\n", resumeAt);
    // Only the audio pipeline restarts. The caller's picture keeps running and
    // re-anchors on the new clock once it answers.
    const bool wasPlaying = playerPlaying;
    return Start(m_path, resumeAt, wasPlaying ? AudioStartState::Playing : AudioStartState::Paused);
}
