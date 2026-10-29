#include "SpeakerEmbedding.h"

#include <windows.h>

// The C API is a single header and no import library: every call is reached
// through the OrtApi table this file looks up at run time. Included here and
// nowhere else, so no other translation unit sees an ONNX type - which is also
// why the player links without onnxruntime.lib and still runs on a machine
// that does not have the DLL.
#pragma warning(push, 0)
#include <onnxruntime_c_api.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

#include "Log.h"
#include "Utf8Text.h"

namespace speaker {
namespace {

using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
static_assert(sizeof(GetApiBaseFn) == sizeof(FARPROC), "a function pointer must fit FARPROC");

// One call either worked or has a reason. Anything else in this file that
// fails would leave a stage reporting "no voices found" without saying why, so
// the reason is written where the notice goes and the caller returns.
bool Check(const OrtApi* api, OrtStatus* status, std::string& notice, const std::string& what)
{
    if (!status) return true;
    notice = what + ": " + (api->GetErrorMessage(status) ? api->GetErrorMessage(status) : "unknown");
    api->ReleaseStatus(status);
    return false;
}

}  // namespace

struct Embedder::Impl {
    HMODULE module{};
    const OrtApi* api{};
    OrtEnv* env{};
    OrtSessionOptions* options{};
    OrtSession* session{};
    OrtMemoryInfo* memory{};
    OrtAllocator* allocator{};
    std::string inputName;
    std::string outputName;

    ~Impl()
    {
        if (api) {
            if (session) api->ReleaseSession(session);
            if (memory) api->ReleaseMemoryInfo(memory);
            if (options) api->ReleaseSessionOptions(options);
            if (env) api->ReleaseEnv(env);
        }
        if (module) FreeLibrary(module);
    }
};

Embedder::Embedder(Engine engine)
{
    if (!engine.Complete()) {
        m_notice = "the speaker model or its runtime is not installed (dub/models, dub/runtime)";
        return;
    }
    auto impl = std::make_unique<Impl>();

    impl->module = LoadLibraryW(engine.library.c_str());
    if (!impl->module) {
        m_notice = "onnxruntime.dll could not be loaded from " +
                   utf8_text::FromWide(engine.library.wstring());
        return;
    }
    // Through memcpy: casting FARPROC straight to a function pointer is what
    // /W4 warns about (C4191) and it is right to.
    const FARPROC entry = GetProcAddress(impl->module, "OrtGetApiBase");
    GetApiBaseFn getApiBase = nullptr;
    std::memcpy(&getApiBase, &entry, sizeof(getApiBase));
    if (!entry || !getApiBase) {
        m_notice = "onnxruntime.dll has no OrtGetApiBase entry point";
        return;
    }
    impl->api = getApiBase()->GetApi(ORT_API_VERSION);
    if (!impl->api) {
        // The header is newer than the DLL beside it. Downgrading the request
        // would run against a vtable this file does not compile for.
        m_notice = "onnxruntime.dll is older than the API this build expects (" +
                   std::to_string(ORT_API_VERSION) + ")";
        return;
    }
    const OrtApi* api = impl->api;

    if (!Check(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "dlss-video-dub", &impl->env), m_notice,
               "the speaker runtime could not start"))
        return;
    if (!Check(api, api->CreateSessionOptions(&impl->options), m_notice,
               "the speaker session options could not be created"))
        return;
    // Same thread count the Python pipeline used: the embedding is a second of
    // audio through a small TDNN, and more threads than cores only adds
    // contention beside the synthesizer.
    unsigned threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    if (!Check(api, api->SetIntraOpNumThreads(impl->options, static_cast<int>(std::min(threads, 8u))),
               m_notice, "the speaker session threads could not be set"))
        return;
    if (!Check(api, api->CreateSession(impl->env, engine.model.c_str(), impl->options, &impl->session),
               m_notice, "the speaker model could not be loaded from " +
                             utf8_text::FromWide(engine.model.wstring())))
        return;
    if (!Check(api, api->GetAllocatorWithDefaultOptions(&impl->allocator), m_notice,
               "the speaker allocator is unavailable"))
        return;
    // The graph's names are read, never assumed: the export has held them
    // steady so far, but a renamed input would otherwise be a run-time
    // "not found" with nothing pointing at the cause.
    for (int which = 0; which < 2; ++which) {
        char* name = nullptr;
        OrtStatus* status = which == 0
            ? api->SessionGetInputName(impl->session, 0, impl->allocator, &name)
            : api->SessionGetOutputName(impl->session, 0, impl->allocator, &name);
        if (!Check(api, status, m_notice, which == 0 ? "the speaker model has no input"
                                                     : "the speaker model has no output"))
            return;
        (which == 0 ? impl->inputName : impl->outputName) = name ? name : "";
        if (name) api->AllocatorFree(impl->allocator, name);
    }
    if (!Check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl->memory),
               m_notice, "the speaker memory info could not be created"))
        return;

    LOG("Speaker: ECAPA embedding ready (" << impl->inputName << " -> " << impl->outputName << ")");
    m_impl = impl.release();
}

Embedder::~Embedder() { delete m_impl; }

std::vector<float> Embedder::Embed(const std::vector<float>& samples)
{
    std::vector<float> vector;
    if (!m_impl) return vector;
    if (samples.size() < static_cast<size_t>(kMinimumSeconds * kSampleRate)) return vector;
    const OrtApi* api = m_impl->api;

    // The tensor borrows this buffer and the API wants it writable, so the
    // caller's vector is copied rather than const_cast.
    std::vector<float> input(samples);
    const int64_t shape[2] = {1, static_cast<int64_t>(input.size())};
    OrtValue* tensor = nullptr;
    std::string ignored;
    if (!Check(api, api->CreateTensorWithDataAsOrtValue(m_impl->memory, input.data(),
                                                        input.size() * sizeof(float), shape, 2,
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tensor),
               ignored, "the speaker tensor could not be built"))
        return vector;

    const char* inputNames[1] = {m_impl->inputName.c_str()};
    const char* outputNames[1] = {m_impl->outputName.c_str()};
    OrtValue* output = nullptr;
    const bool ran = Check(api, api->Run(m_impl->session, nullptr, inputNames, &tensor, 1,
                                         outputNames, 1, &output),
                           ignored, "the speaker embedding could not be computed");
    api->ReleaseValue(tensor);
    if (!ran || !output) return vector;

    OrtTensorTypeAndShapeInfo* info = nullptr;
    if ((api->GetTensorTypeAndShape(output, &info) == nullptr) && info) {
        size_t dimensions = 0;
        api->GetDimensionsCount(info, &dimensions);
        int64_t sizes[4] = {0, 0, 0, 0};
        if (dimensions > 0 && dimensions <= 4 && api->GetDimensions(info, sizes, dimensions) == nullptr) {
            size_t count = 1;
            for (size_t i = 0; i < dimensions; ++i) count *= static_cast<size_t>(sizes[i]);
            void* data = nullptr;
            if (count > 0 && api->GetTensorMutableData(output, &data) == nullptr && data) {
                const float* values = static_cast<const float*>(data);
                vector.assign(values, values + count);
            }
        }
        api->ReleaseTensorTypeAndShapeInfo(info);
    }
    api->ReleaseValue(output);
    if (vector.size() != kDimensions) {
        if (!vector.empty()) LOG("Speaker: the embedding came back " << vector.size() << " wide, not "
                                                                    << kDimensions);
        vector.clear();
        return vector;
    }

    // Unit length, so the clustering below is a dot product and 0.55 means the
    // same thing it meant in step1_align.py.
    double energy = 0.0;
    for (const float value : vector) energy += double(value) * double(value);
    energy = std::sqrt(energy);
    if (!(energy > 0.0)) {
        vector.clear();
        return vector;
    }
    for (float& value : vector) value = static_cast<float>(double(value) / energy);
    return vector;
}

double Similarity(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
    double dot = 0.0;
    for (size_t index = 0; index < a.size(); ++index) dot += double(a[index]) * double(b[index]);
    return dot;
}

}  // namespace speaker
