// Serve a fixed directory with the PRODUCT CastHttpServer implementation.
// Used by the layered DLNA verification: prove that product-encoded HLS
// segments, served by the product HTTP layer with the corrected MIME types,
// are playable on the TV -- independently of the renderer capture path.
//
// Build: cmd /c build-local.cmd cast_http_runner
// Run:   cast_http_runner.exe <directory> [port]   (Ctrl+C or kill to stop)

#include "CastHttpServer.h"
#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: cast_http_runner.exe <directory> [port]\n");
        return 2;
    }
    const std::filesystem::path dir(argv[1]);
    if (!std::filesystem::is_directory(dir)) {
        std::fwprintf(stderr, L"not a directory: %s\n", static_cast<const wchar_t*>(argv[1]));
        return 2;
    }
    unsigned short port = 8080;
    if (argc >= 3) port = static_cast<unsigned short>(_wtoi(argv[2]));

    std::atomic<bool> stop{false};
    LOG("cast_http_runner: serving " << dir.string() << " on 0.0.0.0:" << port);
    std::printf("SERVING %s on %u\n", dir.string().c_str(), port);
    std::fflush(stdout);

    cast_http_server::Settings settings;
    settings.directory = dir;
    settings.port = port;
    settings.stop = [&] { return stop.load(std::memory_order_acquire); };
    const bool ok = cast_http_server::RunServer(settings);
    LOG("cast_http_runner: RunServer returned " << (ok ? "true" : "false"));
    std::printf("RUNSERVER_DONE %d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
