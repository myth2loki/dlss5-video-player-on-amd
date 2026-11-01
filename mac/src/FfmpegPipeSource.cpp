#include "FfmpegPipeSource.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

// ffmpeg and ffprobe are found on PATH. A packaged build will want the bundled
// copies the way the Windows one keeps them beside the exe, but for the port
// this is the whole difference between the two platforms at this layer.
constexpr const char* kFfmpeg = "ffmpeg";
constexpr const char* kFfprobe = "ffprobe";

// Reads a whole pipe to a string. ffprobe's output is a few hundred bytes.
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

// The player's own quoting rule, reduced to POSIX sh: single quotes, with the
// closing-and-reopening dance for an embedded quote. Used only by the ffprobe
// call, which goes through popen and therefore does have a shell.
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

// Fixed-point, no exponent: ffmpeg parses these as plain decimals and an
// ostream's default 6 significant digits would turn 23.976023976 into 23.976.
std::string FormatDouble(double value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

} // namespace

bool FfmpegPipeSource::Probe(const std::string& path, MediaInfo& out)
{
    std::ostringstream command;
    command << kFfprobe << " -v error -select_streams v:0"
            << " -show_entries stream=width,height,r_frame_rate:format=duration"
            << " -of default=noprint_wrappers=1 " << Quote(path);
    const std::string text = RunCapture(command.str());
    if (text.empty()) return false;

    MediaInfo info;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "width") info.width = std::atoi(value.c_str());
        else if (key == "height") info.height = std::atoi(value.c_str());
        else if (key == "duration") info.duration = std::atof(value.c_str());
        else if (key == "r_frame_rate") {
            // "24000/1001" form, which is the one that cannot be recovered by
            // rounding a decimal back out.
            const auto slash = value.find('/');
            if (slash != std::string::npos) {
                const double num = std::atof(value.substr(0, slash).c_str());
                const double den = std::atof(value.substr(slash + 1).c_str());
                if (den != 0.0) info.fps = num / den;
            } else {
                info.fps = std::atof(value.c_str());
            }
        }
    }
    if (!info.Valid()) return false;
    out = info;
    return true;
}

FfmpegPipeSource::~FfmpegPipeSource() { Close(); }

bool FfmpegPipeSource::Open(const std::string& path, const MediaInfo& info)
{
    Close();
    m_path = path;
    m_info = info;
    m_stop = false;
    m_finished = false;
    m_pendingSeek = -1.0;
    m_discardBefore = -1;
    {
        std::scoped_lock lock(m_mutex);
        m_frames.clear();
    }

    // The child is spawned on the producer thread, because it is that thread
    // which owns the pipe descriptor for its whole life.
    m_producer = std::thread([this] { Produce(); });
    return true;
}

void FfmpegPipeSource::Close()
{
    m_stop = true;

    // The producer may be waiting on the queue being full, and that wait's
    // predicate already includes m_stop - but a predicate only runs when
    // something notifies. Setting the flag without notifying leaves the producer
    // asleep for ever, and this join then waits for it: measured as a hang of
    // over sixty seconds on a six-second clip, and it is exactly what would hang
    // the player when the window closes mid-playback.
    //
    // So: set the flag, wake the waiter, shut the pipe and kill the child (which
    // releases a read() blocked on it), THEN join.
    {
        std::scoped_lock lock(m_mutex);
        m_ready.notify_all();
    }
    {
        std::scoped_lock lock(m_pipeMutex);
        if (m_pipe >= 0) {
            ::close(m_pipe);
            m_pipe = -1;
        }
        if (m_pid > 0) {
            ::kill(static_cast<pid_t>(m_pid), SIGKILL);
        }
    }

    if (m_producer.joinable()) m_producer.join();

    std::scoped_lock lock(m_mutex);
    m_frames.clear();
    m_finished = true;
    m_ready.notify_all();
}

bool FfmpegPipeSource::SpawnChild(double startSeconds)
{
    KillChild();

    // argv is built as a vector rather than as a shell string, because
    // posix_spawn does not go through a shell: a quoted path would arrive with
    // its quotes still in it, and ffmpeg would be handed a file name that does
    // not exist. (The Windows side builds a command line and lets
    // CreateProcessW parse it, which is why its quoting and this are not the
    // same code.)
    std::vector<std::string> args;
    // -nostdin matters: without it ffmpeg reads the parent's terminal, and a
    // child that decides to prompt hangs the producer with a full pipe.
    args.push_back("-nostdin");
    args.push_back("-hide_banner");
    args.push_back("-v");
    args.push_back("error");
    if (startSeconds > 0.0) {
        args.push_back("-ss");
        args.push_back(FormatDouble(startSeconds));
    }
    args.push_back("-i");
    args.push_back(m_path);
    // The Windows build's own tail: fixed-rate raw output on the pipe.
    args.push_back("-pix_fmt");
    args.push_back("bgra");
    args.push_back("-fps_mode");
    args.push_back("cfr");
    args.push_back("-r");
    args.push_back(FormatDouble(m_info.fps));
    args.push_back("-f");
    args.push_back("rawvideo");
    args.push_back("pipe:1");

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(kFfmpeg));
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    // stderr goes nowhere: the child is chatty about timestamps, and a full
    // stderr pipe would block it exactly the way a full stdout pipe would.
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, kFfmpeg, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return false;
    }
    {
        std::scoped_lock lock(m_pipeMutex);
        m_pid = static_cast<int>(pid);
        m_pipe = fds[0];
    }
    return true;
}

void FfmpegPipeSource::KillChild()
{
    // Called on the producer thread only, plus once from Close() for the pipe
    // itself (which takes the mutex for those two fields). Everything here is
    // the producer's own teardown, so no lock is needed around the waitpid.
    int pipe = -1;
    int pid = -1;
    {
        std::scoped_lock lock(m_pipeMutex);
        pipe = m_pipe;
        pid = m_pid;
        m_pipe = -1;
        m_pid = -1;
    }
    if (pipe >= 0) ::close(pipe);
    if (pid > 0) {
        // SIGKILL rather than SIGTERM: ffmpeg with a blocked stdout may not reach
        // its signal handler, and a stuck child holds the pipe open.
        ::kill(static_cast<pid_t>(pid), SIGKILL);
        int status = 0;
        ::waitpid(static_cast<pid_t>(pid), &status, 0);
    }
}

void FfmpegPipeSource::Seek(double seconds)
{
    m_pendingSeek = seconds;
    // The producer watches this between frames; a blocked read is released by
    // killing the child, which the producer does on its next pass.
    {
        std::scoped_lock lock(m_mutex);
        m_frames.clear();
    }
    m_ready.notify_all();
}

void FfmpegPipeSource::DiscardBefore(int64_t number) { m_discardBefore = number; }

bool FfmpegPipeSource::TryPop(VideoFrame& out)
{
    std::scoped_lock lock(m_mutex);
    if (m_frames.empty()) return false;
    out = std::move(m_frames.front());
    m_frames.pop_front();
    m_ready.notify_all();
    return true;
}

bool FfmpegPipeSource::PeekFrontNumber(int64_t& number) const
{
    std::scoped_lock lock(m_mutex);
    if (m_frames.empty()) return false;
    number = m_frames.front().number;
    return true;
}

bool FfmpegPipeSource::Pop(VideoFrame& out)
{
    std::unique_lock lock(m_mutex);
    m_ready.wait(lock, [this] { return !m_frames.empty() || m_finished; });
    if (m_frames.empty()) return false;
    out = std::move(m_frames.front());
    m_frames.pop_front();
    lock.unlock();
    m_ready.notify_all();
    return true;
}

void FfmpegPipeSource::Produce()
{
    std::vector<uint8_t> buffer;
    const size_t frameBytes = m_info.FrameBytes();
    buffer.resize(frameBytes);

    int64_t nextNumber = 0;

    if (!SpawnChild(0.0)) {
        std::scoped_lock lock(m_mutex);
        m_finished = true;
        m_ready.notify_all();
        return;
    }

    while (!m_stop) {
        // A seek request re-spawns the child at the new position; the frame
        // numbering restarts from that position's own index so a consumer can
        // tell what it is looking at without a second timeline.
        const double seekTo = m_pendingSeek.exchange(-1.0);
        if (seekTo >= 0.0) {
            nextNumber = static_cast<int64_t>(std::llround(seekTo * m_info.fps));
            if (!SpawnChild(seekTo)) {
                std::scoped_lock lock(m_mutex);
                m_finished = true;
                m_ready.notify_all();
                return;
            }
        }

        // A local copy, because Close() may set m_pipe to -1 under the mutex
        // while this thread is between reads. Reading a closed descriptor would
        // return EBADF and be mistaken for the end of the stream.
        int pipe = -1;
        {
            std::scoped_lock lock(m_pipeMutex);
            pipe = m_pipe;
        }
        if (pipe < 0) break;

        size_t filled = 0;
        bool shortRead = false;
        while (filled < frameBytes) {
            const ssize_t got = ::read(pipe, buffer.data() + filled, frameBytes - filled);
            if (got > 0) { filled += static_cast<size_t>(got); continue; }
            if (got == 0) { shortRead = true; break; }          // child exited
            if (errno == EINTR) continue;
            shortRead = true;
            break;
        }
        if (shortRead) {
            // End of stream, or the child was killed for a seek. Either way
            // this frame is not whole and must not reach a consumer.
            std::scoped_lock lock(m_mutex);
            if (m_pendingSeek.load() < 0.0) {
                m_finished = true;
                m_ready.notify_all();
            }
            if (m_finished) return;
            continue;
        }

        const int64_t number = nextNumber++;
        const int64_t discard = m_discardBefore.load();
        if (discard >= 0 && number < discard) continue;

        std::unique_lock lock(m_mutex);
        // Bounded so a paused consumer does not let the child run the whole
        // file into memory. The producer waits, which back-pressures ffmpeg
        // through the pipe rather than dropping frames.
        m_ready.wait(lock, [this] {
            return m_frames.size() < kQueueLimit || m_stop || m_pendingSeek.load() >= 0.0;
        });
        if (m_stop || m_pendingSeek.load() >= 0.0) continue;
        VideoFrame frame;
        frame.bgra = buffer;      // a copy: `buffer` is reused for the next read
        frame.number = number;
        m_frames.push_back(std::move(frame));
        lock.unlock();
        m_ready.notify_all();
    }

    KillChild();
    std::scoped_lock lock(m_mutex);
    m_finished = true;
    m_ready.notify_all();
}
