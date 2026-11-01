#pragma once

#include "PlatformCompat.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// What the player says, and remembers, about loadable modules in the neural
// runtime directory. FindUnlockedRuntimeModules (RuntimeLock.h) finds them;
// this is the wording and the identity built on top of it, shared by the
// player and the helper so the two can never describe one directory
// differently.
namespace runtime_modules {

// The refusal for a directory holding modules the runtime lock does not name,
// `names` as FindUnlockedRuntimeModules returned them. Empty when there are
// none. The helper refuses with this text at startup; the player now refuses
// with it BEFORE launching the helper, which cost a helper start and a
// preflight to reach the same sentence.
inline std::wstring UnlockedModulesRefusal(const std::vector<std::wstring>& names)
{
    if (names.empty()) return {};
    std::wstring joined;
    for (const std::wstring& name : names) {
        if (!joined.empty()) joined += L", ";
        joined += name;
    }
    return L"The neural runtime directory holds modules the runtime lock does not name, which the helper "
           L"would load: " + joined + L". Remove them and try again.";
}

inline bool IsModuleExtension(std::wstring extension)
{
    for (wchar_t& c : extension)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return extension == L".dll" || extension == L".addon" || extension == L".addon32" || extension == L".addon64";
}

// Every module the loader or the ReShade proxy could pick up from the top of
// `runtimeDirectory` - name, size and write time - in one comparable string.
// A refused preflight is remembered for the session, and it was remembered
// under the runtime digest alone: that hashes only the locked files, so
// removing the stray add-on that caused the refusal left the refusal standing
// until the player restarted. With this in the key the refusal ends when the
// modules change. Only modules: ReShade.ini is rewritten for every job and the
// logs for every run, and a key that moved with them would re-probe every time.
// Empty when the directory cannot be listed, which never equals a listing.
inline std::wstring ModuleListingIdentity(const std::filesystem::path& runtimeDirectory)
{
    std::vector<std::wstring> modules;
    std::error_code error;
    std::filesystem::directory_iterator it(runtimeDirectory, error);
    if (error) return {};
    for (const std::filesystem::directory_iterator end{}; it != end; it.increment(error)) {
        if (error) return {};
        std::error_code typeError;
        if (!it->is_regular_file(typeError) || typeError) continue;
        if (!IsModuleExtension(it->path().extension().wstring())) continue;
        std::error_code sizeError, timeError;
        const auto size = it->file_size(sizeError);
        const auto written = it->last_write_time(timeError);
        std::wstring name = it->path().filename().wstring();
        for (wchar_t& c : name)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        // The two casts below are a port fix, not formatting taste.
        //
        // `timeError ? 0 : written.time_since_epoch().count()` has type
        // long long on Windows, where count() is long long, so to_wstring sees
        // one candidate. On LP64 count() is `long`, the conditional's two arms
        // have different types, and the result is `long` - which makes
        // to_wstring ambiguous between its long and long long overloads. The
        // cast states the width instead of leaving it to the conditional's
        // arithmetic, and the digest this feeds is a cache key: it has to be
        // the same string on every platform or a render is never reused.
        const std::int64_t sizeValue = sizeError ? 0 : static_cast<std::int64_t>(size);
        const std::int64_t timeValue =
            timeError ? 0 : static_cast<std::int64_t>(written.time_since_epoch().count());
        modules.push_back(name + L"|" + std::to_wstring(sizeValue) + L"|" +
                          std::to_wstring(timeValue));
    }
    std::sort(modules.begin(), modules.end());
    std::wstring identity = L"modules:";
    for (const std::wstring& module : modules) identity += module + L";";
    return identity;
}

} // namespace runtime_modules
