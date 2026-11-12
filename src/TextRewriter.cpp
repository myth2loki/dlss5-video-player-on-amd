#include "TextRewriter.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "DubFitPolicy.h"
#include "EngineChild.h"
#include "Log.h"
#include "Utf8Text.h"

namespace dub {
namespace {

// Beside the translator's own port, so the two can run at once - the Convert
// video dialog translates first and dubs second, and starting this engine while
// the other one is still shutting down must not collide.
constexpr int kPort = 8131;

// How many characters the pace model says fit `seconds`. The same model the fit
// stage trusts, inverted: seconds = perChar * characters + perComma * commas +
// base. A comma is assumed to survive the rewrite, so one is budgeted for.
int CharactersFor(double seconds)
{
    const auto& pace = dub_fit::kQwen3TtsPace;
    const double room = seconds - pace.base - pace.perComma;
    if (room <= 0.0) return 3;
    return int(std::clamp(room / pace.perChar, 3.0, 60.0));
}

std::string Prompt(const std::string& text, double seconds, int limit)
{
    std::string prompt;
    prompt += "你是电影配音的台词压缩助手。把下面这句中文改写得更短，让它能在 ";
    char number[32];
    std::snprintf(number, sizeof(number), "%.1f", seconds);
    prompt += number;
    prompt += " 秒内念完（不超过 ";
    prompt += std::to_string(limit);
    prompt += " 个字）。要求：保留原意和语气，不要解释，不要引号，不要加注释，只输出改写后的中文句子。\n";
    prompt += "原句：";
    prompt += text;
    prompt += "\n改写：";
    return prompt;
}

// The mirror of Prompt: the same sentence, with room to unfold. The instruction
// that matters is the one forbidding new facts - asked to "补足细节", a 4B model
// produced a "[说话人姓名]" placeholder and a 17-character line became 108
// characters. Only same-meaning expansion is allowed here; FillIsSafe checks the
// numbers afterwards because the prompt alone does not hold.
std::string ExpandPrompt(const std::string& text, int target)
{
    std::string prompt;
    prompt += "你是电影配音的台词改写助手。下面这句中文配音太短，念完之后句尾会空一块。";
    prompt += "请把它扩写到 ";
    prompt += std::to_string(target);
    prompt += " 个字左右（不超过 ";
    prompt += std::to_string(target + target / 10);
    prompt += " 个字）。要求：意思完全不变，不得新增任何事实、数字、人名、地名或结论；";
    prompt += "只许同义展开——把概括的说法说细一点、把短语拆成短句、补「其实」「所以说」「你会发现」这类连接语；";
    prompt += "不要重复同一句，不要分点，不要解释，不要引号或注释，只输出改写后的中文句子。\n";
    prompt += "原句：";
    prompt += text;
    prompt += "\n改写：";
    return prompt;
}

// What the model answered, cleaned: a rewrite is one line of Chinese, and
// anything else - a preamble, a translation note, an empty string, a repeat of
// the original - is not a rewrite and is thrown away.
std::string OneLine(const std::string& answer)
{
    std::string cleaned;
    for (const char c : answer) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!cleaned.empty()) break;  // the first line is the answer
            continue;
        }
        if (cleaned.empty() && (c == ' ' || c == '\t')) continue;
        cleaned += c;
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '\t')) cleaned.pop_back();
    // Quotes the model likes to wrap the line in are not part of the line.
    for (const std::string quote : {"\"", "'", "「", "」", "“", "”", "*"}) {
        while (cleaned.size() >= quote.size() && cleaned.rfind(quote, 0) == 0)
            cleaned = cleaned.substr(quote.size());
        while (cleaned.size() >= quote.size() &&
               cleaned.compare(cleaned.size() - quote.size(), quote.size(), quote) == 0)
            cleaned.resize(cleaned.size() - quote.size());
    }
    return cleaned;
}

}  // namespace

struct Rewriter::Impl {
    engine_child::Handle job;
    engine_child::Handle process;
    engine_child::Handle session;
    engine_child::Handle connection;

    ~Impl()
    {
        // The process first: a server holding its port while the connection is
        // closed would answer the next stage's request from the previous run.
        if (process) {
            TerminateProcess(process.Get(), 0);
            WaitForSingleObject(process.Get(), 5000);
        }
        connection.Reset();
        session.Reset();
        // The job goes last, and killing it takes any child the server forked.
        job.Reset();
    }
};

Rewriter::Rewriter(subtitle_translate::Engine engine, bool enabled)
{
    if (!enabled || engine.model.empty()) return;
    const auto server = engine.runtimeDirectory / L"llama-server.exe";
    std::error_code error;
    if (!std::filesystem::exists(server, error)) {
        LOG("Rewrite: no llama-server at " << utf8_text::FromWide(server.wstring())
                                           << "; lines that miss their slot keep what they have (tempo, then a cut)");
        return;
    }
    auto impl = std::make_unique<Impl>();
    impl->job.Reset(CreateJobObjectW(nullptr, nullptr));
    if (impl->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl->job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    const std::wstring arguments = subtitle_translate::ServerArguments(
        engine.model.wstring(), kPort, engine.gpuLayers, engine.threads,
        subtitle_translate::ChatTemplateFor(utf8_text::FromWide(engine.model.filename().wstring())));
    engine_child::Child child;
    if (!engine_child::Spawn(server, arguments, impl->job.Get(), false, child, "llama-server (rewrite)")) {
        LOG("Rewrite: the engine could not be started; lines that miss their slot keep what they have");
        return;
    }
    impl->process = std::move(child.process);

    impl->session.Reset(WinHttpOpen(L"DLSSVideoPlayer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!impl->session) {
        LOG("Rewrite: WinHttpOpen failed winerr=" << GetLastError());
        return;
    }
    WinHttpSetTimeouts(impl->session.Get(), 2000, 2000, 30000, 120000);
    impl->connection.Reset(WinHttpConnect(impl->session.Get(), L"127.0.0.1", INTERNET_PORT(kPort), 0));
    if (!impl->connection) {
        LOG("Rewrite: WinHttpConnect(" << kPort << ") failed winerr=" << GetLastError());
        return;
    }

    // A 4B model reads its weights before it answers, and answers 503 while it
    // does: waiting, not failing.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int64_t(engine.startTimeoutSeconds * 1000.0));
    const auto began = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        const auto answer = engine_child::HttpRequest(impl->connection.Get(), L"GET", "/health", {}, {}, 4000);
        if (answer && answer->status == 200) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            LOG("Rewrite: llama-server ready in " << seconds << " s");
            m_impl = impl.release();
            return;
        }
        if (WaitForSingleObject(impl->process.Get(), 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(impl->process.Get(), &code);
            LOG("Rewrite: llama-server exited with " << code << "; lines that miss their slot keep what they have");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOG("Rewrite: llama-server never answered /health; lines that miss their slot keep what they have");
}

Rewriter::~Rewriter() { delete m_impl; }

std::string Rewriter::Shorten(const std::string& text, double seconds)
{
    if (!m_impl || text.empty() || seconds <= 0.4) return {};
    const int limit = CharactersFor(seconds);
    const std::string body = subtitle_translate::ChatRequestBody(Prompt(text, seconds, limit), 192);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto answer = engine_child::HttpRequest(m_impl->connection.Get(), L"POST",
                                                      "/v1/chat/completions", body, "application/json", 60000);
        if (!answer || answer->status != 200) continue;
        const auto content = subtitle_translate::JsonStringField(answer->body, "content");
        if (!content) continue;
        const std::string rewritten = OneLine(*content);
        if (rewritten.empty() || rewritten == text) continue;
        // Longer than asked for is not a rewrite, and the caller would only pay
        // for a second synthesis to arrive where it started.
        size_t characters = 0;
        for (const unsigned char lead : rewritten) characters += (lead & 0xC0) != 0x80 ? 1 : 0;
        if (characters >= size_t(limit) + 4) {
            LOG("Rewrite: the engine answered " << characters << " characters for a " << limit
                                                << "-character budget; keeping the original");
            continue;
        }
        LOG("Rewrite: " << text << "  ->  " << rewritten << "  (" << characters << " characters for "
                        << limit << ")");
        return rewritten;
    }
    return {};
}

std::string Rewriter::Lengthen(const std::string& text, std::size_t characters)
{
    if (!m_impl || text.empty() || characters == 0) return {};
    // The model is asked for a size, and a film's longest sentence is nowhere
    // near 400 characters of Chinese; the clamp is there so a bad pace model
    // cannot turn into an unbounded generation.
    const int target = int(std::min<std::size_t>(characters, 400));
    const std::string body = subtitle_translate::ChatRequestBody(ExpandPrompt(text, target), 256);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto answer = engine_child::HttpRequest(m_impl->connection.Get(), L"POST",
                                                      "/v1/chat/completions", body, "application/json", 60000);
        if (!answer || answer->status != 200) continue;
        const auto content = subtitle_translate::JsonStringField(answer->body, "content");
        if (!content) continue;
        const std::string rewritten = OneLine(*content);
        if (rewritten.empty() || rewritten == text) continue;
        if (!dub_fit::FillIsSafe(text, rewritten, characters)) {
            std::size_t before = 0;
            std::size_t after = 0;
            std::size_t commas = 0;
            dub_fit::MeasureText(text, before, commas);
            dub_fit::MeasureText(rewritten, after, commas);
            LOG("Rewrite: answered " << after << " characters for a " << characters
                                     << "-character fill of " << before
                                     << "; keeping the original");
            continue;
        }
        std::size_t filled = 0;
        std::size_t commas = 0;
        dub_fit::MeasureText(rewritten, filled, commas);
        LOG("Rewrite: " << text << "  ->  " << rewritten << "  (" << filled << " characters for "
                        << characters << ")");
        return rewritten;
    }
    return {};
}

}  // namespace dub
