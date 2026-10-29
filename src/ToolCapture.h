#pragma once

// Runs one of the staged FFmpeg tools and hands back what it wrote to stdout,
// or nothing when it could not start, failed, overran `limit` bytes or ran past
// `timeout`. For small questions with small answers only - a chapter list, one
// thumbnail, a subtitle track dumped as text - so it holds the whole answer in
// memory and never asks the user anything. The child is in a kill-on-close job,
// so a caller that goes away mid-question takes it along.
//
// Pulled out of main.cpp so more than the player asks these questions: the
// Convert video dialog's burn path (ConvertTextStage.h) extracts a source's own
// subtitle track through the same door.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "HardErrorSuppression.h"
#include "InheritedHandles.h"
#include "RuntimePolicy.h"

inline std::optional<std::string> RunToolCapture(const std::filesystem::path& executable,
                                                 const std::vector<std::wstring>& arguments,
                                                 std::stop_token stop, std::chrono::milliseconds timeout,
                                                 size_t limit)
{
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE readPipe=nullptr,writePipe=nullptr;
    if(!CreatePipe(&readPipe,&writePipe,&security,0))return std::nullopt;
    SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(job){JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));}
    const InheritedHandles inherit{writePipe};
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput=writePipe;startup.lpAttributeList=inherit.AttributeList();
    PROCESS_INFORMATION process{};
    std::wstring commandLine=BuildWindowsCommandLine(executable.native(),arguments);
    BOOL created=FALSE;
    {const ScopedHardErrorSuppression noHardErrorDialog;
     created=inherit.Ready()&&CreateProcessW(executable.c_str(),commandLine.data(),nullptr,nullptr,inherit.InheritHandles(),
                            CREATE_NO_WINDOW|CREATE_SUSPENDED|inherit.CreationFlags(),
                            nullptr,executable.parent_path().c_str(),&startup.StartupInfo,&process);}
    CloseHandle(writePipe);
    if(!created){CloseHandle(readPipe);if(job)CloseHandle(job);return std::nullopt;}
    if(job&&!AssignProcessToJobObject(job,process.hProcess)){CloseHandle(job);job=nullptr;}
    ResumeThread(process.hThread);CloseHandle(process.hThread);
    std::string output;bool failed=false;
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    char buffer[16384];
    for(bool exited=false;;){
        DWORD available=0;
        while(PeekNamedPipe(readPipe,nullptr,0,nullptr,&available,nullptr)&&available){
            DWORD read=0;
            if(!ReadFile(readPipe,buffer,std::min<DWORD>(available,DWORD(sizeof(buffer))),&read,nullptr)||!read)break;
            if(output.size()+read>limit){failed=true;break;}
            output.append(buffer,read);
        }
        if(failed||exited)break;
        exited=WaitForSingleObject(process.hProcess,10)==WAIT_OBJECT_0;
        if(!exited&&(stop.stop_requested()||std::chrono::steady_clock::now()>=deadline)){failed=true;break;}
    }
    DWORD exitCode=1;
    if(failed){if(job)TerminateJobObject(job,1);else TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,2000);}
    else GetExitCodeProcess(process.hProcess,&exitCode);
    CloseHandle(process.hProcess);CloseHandle(readPipe);if(job)CloseHandle(job);
    if(failed||exitCode!=0)return std::nullopt;
    return output;
}
