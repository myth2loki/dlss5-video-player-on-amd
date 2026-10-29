// Minimal test to invoke the convert video pipeline programmatically
// Compile with: g++ -std=c++20 -I./src -I./include test_convert.cpp -o test_convert.exe \
//   ./build-ninja/*.lib -lffmpeg -lavcodec -lavformat -lavutil -lswresample -lavfilter

#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <stop_token>
#include <iostream>

// Include necessary headers
#include "Dubbing.h"
#include "MediaPipeline.h"
#include "StageExport.h"
#include "ExportPipeline.h"
#include "SubtitleTranslator.h"
#include "SpeechToText.h"
#include "Localization.h"
#include "PlatformPaths.h"
#include "DubFitPolicy.h"

namespace fs = std::filesystem;

int main() {
    // Setup paths
    fs::path source = R"(F:/dlss5-video/test-media/tears_of_steel_720p.mov)";
    fs::path outDir = R"(F:/dlss5-video/dlss5-video-amd/run-fg/test_output)";
    fs::path scratch = outDir / "scratch";
    fs::create_directories(scratch);
    fs::create_directories(outDir);
    
    fs::path output = outDir / "test_dub.mkv";
    
    std::cout << "Source: " << source << std::endl;
    std::cout << "Output: " << output << std::endl;
    std::cout << "Scratch: " << scratch << std::endl;
    
    // Build dub plan (similar to StartConvertVideo)
    dub::Plan dubPlan;
    {
        // Find dub engine files
        fs::path dubRoot = fs::path(R"(F:/dlss5-video/dlss5-video-amd/dub)");
        fs::path runtimeDir = dubRoot / "runtime";
        fs::path runner = runtimeDir / "llama-tts.exe";
        fs::path model, mmproj;
        
        for (auto& p : fs::directory_iterator(dubRoot / "models")) {
            if (p.path().extension() == ".gguf") {
                std::wstring name = p.path().filename().wstring();
                if (name.rfind(L"mmproj", 0) == 0) {
                    if (mmproj.empty()) mmproj = p.path();
                } else {
                    if (model.empty()) model = p.path();
                }
            }
        }
        
        if (model.empty() || mmproj.empty() || !fs::exists(runner)) {
            std::cerr << "Missing dub engine files!" << std::endl;
            return 1;
        }
        
        dubPlan.engine.runner = runner;
        dubPlan.engine.model = model;
        dubPlan.engine.mmproj = mmproj;
        dubPlan.helpers = fs::path(R"(F:/dlss5-video/dlss5-video-amd/run-fg)");
        dubPlan.work = scratch / "dub";
        dubPlan.logFile = dubPlan.work / "dub.log";
        dubPlan.mediaPath = source;
        dubPlan.track = dubPlan.work / "dub-track.wav";
        // Duration will be filled by ffprobe
        dubPlan.durationSeconds = 0; // Will be determined
        dubPlan.language = "zho"; // Chinese
        
        std::cout << "Dub engine: " << runner << std::endl;
        std::cout << "Model: " << model << std::endl;
        std::cout << "MMProj: " << mmproj << std::endl;
    }
    
    // First, need to run translation to get ASS file
    // This requires the full translation pipeline...
    std::cout << "Need to run translation first to get ASS file for dub" << std::endl;
    
    // For now, test dub directly if we have an ASS file
    // Let's check if we can generate one from the source
    // The source tears_of_steel_720p.mov likely has English subtitles
    
    return 0;
}
