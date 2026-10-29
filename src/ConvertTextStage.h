#pragma once

// The text stage of a Convert video run: the transcript-and-translate step that
// writes the subtitle file the finished container carries, and which the dub
// reads its lines from. Pulled out of main.cpp verbatim so a headless caller
// (tests/DubConvertE2E.cpp) drives exactly the code the dialog drives - one
// pipeline, one set of refusals, whichever way a conversion is started.
//
// Built on the thread that owns the settings and the media's own track list;
// the worker only reads it. RunConvertVideoText blocks until the stage is done
// or cancelled - it is minutes of work, and `stop` is the Cancel.
//
// Nothing is generated when the source's own track is the one being carried:
// the mux already copies the source's subtitle streams, so "subtitles, not
// translated" is the export that has always happened and needs no engine at
// all. Only a translated track is work.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>

#include "SpeechToText.h"
#include "SubtitleTranslator.h"
#include "ToolCapture.h"

struct ConvertTextPlan{
    bool needed{};       // a translated track was asked for
    bool transcribe{};   // the source carries no text: listen to the speech first
    bool extract{};      // burn without translating: dump the source's own track to a file the ass filter can read
    int stream{};        // the source's own text track, when there is one
    std::wstring mediaPath;
    std::filesystem::path extractFile;
    std::wstring identity;
    subtitle_speech::Engine speech;
    subtitle_translate::Engine translation;
    std::filesystem::path helpers;
    std::filesystem::path cacheDirectory;
    subtitle_speech::Job speechJob;
    subtitle_translate::Languages languages;
    uint32_t width{1920},height{1080};
    // The transcript, then the translation. A source that already has words
    // skips the first.
    uint32_t Phases()const{return transcribe?2u:1u;}
};
struct ConvertTextResult{
    bool ok{};
    bool cancelled{};
    std::filesystem::path file;
    std::wstring detail;
};

// Runs the stage and blocks until it is done or cancelled.
//
// Both engines are local to the run. The player has its own pair for the
// subtitles on screen, and a conversion that drove those would publish its
// transcript over what the viewer is watching and cancel on the next seek.
inline ConvertTextResult RunConvertVideoText(const ConvertTextPlan& plan,std::stop_token stop,
    const std::function<void(const wchar_t*,uint64_t,uint64_t)>& report)
{
    ConvertTextResult result;
    if(plan.extract){
        // The source's own track, as a file the mux's ass filter can draw:
        // one ffmpeg demux, no engine and no model. It is the whole film's
        // timeline, which is why a ranged burn is refused before this runs.
        const auto output=RunToolCapture(plan.helpers/L"ffmpeg.exe",
            {L"-hide_banner",L"-nostdin",L"-loglevel",L"error",L"-xerror",L"-y",
             L"-i",plan.mediaPath,L"-map",L"0:s:"+std::to_wstring(plan.stream),
             L"-f",L"ass",plan.extractFile.wstring()},
            stop,std::chrono::minutes(10),64*1024);
        if(stop.stop_requested()){result.cancelled=true;return result;}
        if(!output){
            result.detail=L"The source's subtitle track could not be extracted for burning.";
            return result;
        }
        result.file=plan.extractFile;result.ok=true;return result;
    }
    std::filesystem::path cues;
    if(plan.transcribe){
        subtitle_speech::SpeechToText speech(plan.helpers,plan.speech,plan.cacheDirectory);
        // Batch, not live: SetPlayhead is never called, so the playhead stays
        // negative, WaitForLead returns at once and every window of the film
        // is heard rather than only what is ahead of a viewer who is not
        // watching - there is no playback to stay ahead of here.
        speech.Start(plan.speechJob);
        for(;;){
            if(stop.stop_requested()){speech.Cancel();result.cancelled=true;return result;}
            const auto status=speech.Poll();
            if(report)report(L"convert.progress.transcript",uint64_t(status.chunksDone),
                             uint64_t(status.chunksTotal?status.chunksTotal:1));
            if(!status.Running()){
                if(status.state!=subtitle_speech::State::Done||status.finalTrack.empty()){
                    result.detail=status.notice.empty()
                        ?L"The speech could not be transcribed.":status.notice;
                    return result;
                }
                cues=status.finalTrack;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    subtitle_translate::Translator translator(plan.helpers,plan.translation,plan.cacheDirectory);
    subtitle_translate::Job job;
    if(plan.transcribe){
        // What listening produced. The translator reads a file, not a stream,
        // and an SRT has one stream of its own, so the two spellings meet here.
        job.path=cues.wstring();job.stream=0;job.external=true;
    }else{
        job.path=plan.mediaPath;job.stream=plan.stream;job.external=false;
    }
    job.identity=plan.identity;
    job.languages=plan.languages;
    job.width=plan.width;job.height=plan.height;
    translator.Start(job);
    for(;;){
        if(stop.stop_requested()){translator.Cancel();result.cancelled=true;return result;}
        const auto status=translator.Poll();
        if(report)report(L"convert.progress.translate",uint64_t(status.done),
                         uint64_t(status.total?status.total:1));
        if(!status.Running()){
            if(status.state==subtitle_translate::State::Done&&!status.result.empty()){
                result.file=status.result;result.ok=true;
            }else{
                result.detail=status.notice.empty()
                    ?L"The subtitles could not be translated.":status.notice;
            }
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
