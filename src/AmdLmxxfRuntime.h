#pragma once

#include "AmdLmxxfApi.h"
#include "../third_party/mochizuki/MochizukiNrApi.h"
#include "../third_party/mochizuki/MochizukiNrControls.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace amd_lmxxf {

// Which runtime tree a session is brought up against. The two share one API
// table (LmxxfNrApi, ABI 6, 176 bytes) and differ in exactly two places: the
// module that hands the table back, and the export that prepares a frame -
// Mochizuki's table entry is a rejection stub and its frame struct has its own
// layout. Everything else in this file serves both unchanged.
enum class NrKind
{
    Lmxxf,
    Mochizuki,
};

std::wstring_view NrKindName(NrKind kind) noexcept;
// The module each kind lives in. Coexistence is two DLLs in one directory, so
// this is the only thing that decides which of them a session talks to.
const wchar_t* NrKindModuleName(NrKind kind) noexcept;
// LmxxfNrGetApi / MochizukiNrGetApi. Both take (abi_version, LmxxfNrApi*) and
// pre-stamp struct_size, so the negotiation loop below is kind-independent.
const char* NrKindGetApiExport(NrKind kind) noexcept;

enum class RuntimeFailure
{
    None,
    MissingModule,
    MissingExport,
    GetApiFailed,
    InvalidApiTable,
    InvalidCapabilities,
    CreateFailed,
    PrepareFailed,
};

constexpr std::wstring_view RuntimeFailureName(RuntimeFailure failure) noexcept
{
    switch (failure) {
        case RuntimeFailure::None: return L"none";
        case RuntimeFailure::MissingModule: return L"missing-module";
        case RuntimeFailure::MissingExport: return L"missing-export";
        case RuntimeFailure::GetApiFailed: return L"get-api-failed";
        case RuntimeFailure::InvalidApiTable: return L"invalid-api-table";
        case RuntimeFailure::InvalidCapabilities: return L"invalid-capabilities";
        case RuntimeFailure::CreateFailed: return L"create-failed";
        case RuntimeFailure::PrepareFailed: return L"prepare-failed";
    }
    return L"unknown";
}

// Where the requested runtime and its module tree are expected to be. The same
// search order the ffmpeg helpers use (MediaTools.h / FindToolIn) and for the
// same reason: an explicit answer wins, then the layouts a checkout and a
// packaged build actually have. Empty when none of them holds the module, which
// the caller reports rather than papering over - a job that silently ran against
// a different runtime tree would make every other check meaningless.
//
// `kind` selects which module has to be present: a directory holding only
// LmxxfNrRuntime.dll is not a Mochizuki tree, and vice versa. The default keeps
// every existing caller on the lmxxf answer it has always received.
//
// The environment variable is named after the compile definition the AMD tests
// build against, so one name means one thing across both.
std::filesystem::path ResolveRuntimeDirectory(const std::filesystem::path& explicitDirectory = {},
                                              NrKind kind = NrKind::Lmxxf);

// Owns one loaded LmxxfNrRuntime module and at most one live context. The
// runtime globally records its last error, so all calls on one module must be
// serialized by the owning worker or evaluator.
class Runtime
{
public:
    Runtime() = default;
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;

    // `runtimeDirectory` must contain the module for `kind`. Reparse points and
    // directories are refused so a checked package cannot redirect the load
    // outside the runtime tree.
    //
    // The one-argument form is the lmxxf load it has always been; the two-argument
    // form is the same load against the named runtime tree.
    bool Load(const std::filesystem::path& runtimeDirectory, std::wstring* error);
    bool Load(const std::filesystem::path& runtimeDirectory, NrKind kind, std::wstring* error);
    void Unload();

    bool QueryCapabilities(std::wstring* error);
    bool CreateSession(void* device, void* queue, const std::filesystem::path& assetsDirectory,
                       uint32_t flags, std::wstring* error);
    bool PrepareSession(std::wstring* error);
    void ResetSession();

    bool Loaded() const noexcept { return module_ != nullptr; }
    bool SessionCreated() const noexcept { return context_ != nullptr; }
    bool TimingAvailable() const noexcept { return api_.GetTimings != nullptr; }

    NrKind Kind() const noexcept { return kind_; }
    // Non-null only for a Mochizuki load. The table's own PrepareFrame is a
    // rejection stub on that runtime, so a session built against one must call
    // this export and nothing else for the prepare step.
    PFN_MochizukiNrPrepareFrame MochizukiPrepare() const noexcept { return mochizukiPrepare_; }
    // The build-progress report, resolved with the prepare export on the same
    // load. Absent is not a failure: a runtime without it still renders, the host
    // just has nothing to show while the network compiles.
    bool BuildProgress(MochizukiNrBuildProgress& out) const;
    // The model's picture controls on the Mochizuki path. On lmxxf the same
    // questions are answered by a configuration file the network is compiled
    // from, which is why that backend reloads the module to change its style;
    // here it is session state and a moved value rebuilds the network in the
    // background. False when the runtime does not export it.
    bool SetControls(const MochizukiNrControls& controls, std::wstring* error);
    // The size this package's MochizukiNrControls actually has, discovered once
    // from GetControlDefaults (see the .cpp). 0 until probed, and 0 when the
    // runtime exports no control surface at all.
    uint32_t MochizukiControlsSize();
    // What the session is doing: extents, passes, the last build's duration, and
    // whether the ACO import actually took rather than being merely requested.
    bool Info(MochizukiNrInfo& out) const;
    // The runtime's own diagnostics, off by default: it writes them to
    // mochizuki_nr.log beside dlssnr-amd and rotates that at 4 MiB. A failure this
    // host can only see as a status code - "the network would not build" - has its
    // reason in that file, so bring-up turns it on.
    bool EnableLogging(bool enabled);

    const LmxxfNrApi& Api() const noexcept { return api_; }
    const LmxxfNrCapabilities& Capabilities() const noexcept { return capabilities_; }
    void* Context() const noexcept { return context_; }
    const std::filesystem::path& ModulePath() const noexcept { return modulePath_; }

    std::string LastError() const;
    std::string Status() const;
    bool GetTimings(LmxxfNrTimings& out, std::string* error);

private:
    void ResetHandles() noexcept;
    void MoveFrom(Runtime& other) noexcept;

    HMODULE module_{};
    LmxxfNrApi api_{};
    LmxxfNrCapabilities capabilities_{};
    void* context_{};
    NrKind kind_{NrKind::Lmxxf};
    PFN_MochizukiNrPrepareFrame mochizukiPrepare_{};
    PFN_MochizukiNrGetBuildProgress mochizukiProgress_{};
    int32_t (*mochizukiSetControls_)(void*, const MochizukiNrControls*){};
    int32_t (*mochizukiGetControlDefaults_)(MochizukiNrControls*){};
    int32_t (*mochizukiGetInfo_)(void*, MochizukiNrInfo*){};
    int32_t (*mochizukiGetFeatures_)(){};
    void (*mochizukiSetLogging_)(uint32_t){};
    // Learned from the package once, like the controls payload size: the header
    // vendored here is a later revision than 1.12.1, and GetInfo rejects any
    // struct_size but the package's own.
    mutable uint32_t mochizukiInfoSize_{};
    uint32_t mochizukiControlsSize_{};
    std::wstring assetsDirectory_;
    std::filesystem::path modulePath_;
};

} // namespace amd_lmxxf
