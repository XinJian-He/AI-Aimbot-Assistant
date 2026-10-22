#pragma once
#ifndef AIMLIB_BUILD
#define AIMLIB_BUILD
#endif
#include "aim/aimapi.h"
#include "resource_monitor.h"
#include "../aim/recent_log.h"
#include <map>
#include <string>
#include <vector>
namespace NativeBackend {
using Parameters = std::map<std::string,std::string>;
struct Result {
    bool ok = false;
    bool replaceDraft = false;
    bool persisted = false;
    unsigned connectionId = 0;
    std::string message;
    Parameters confirmed;
    std::string devicesJson, modesJson, classesJson;
    unsigned generation=0;
    std::string device;
};
struct State {
    bool running=false, paused=true, connected=false, captureFailed=false, idle=false;
    int moveMethod=-1, captureBackend=-1;
    AimDeviceConnection connection{};
    AimTelemetry telemetry{};
    ResourceUsage resources;
};
struct Snapshot {
    unsigned revision=0;
    State state;
    Parameters parameters;
    std::deque<RecentLog::Entry> logs;
    std::vector<std::wstring> profiles,models;
    std::string error;
};
Snapshot ReadSnapshot();
Result CaptureDevices(unsigned generation);
Result CaptureModes(const std::string& device,unsigned generation);
std::wstring Directory();
Parameters ReadParameters();
State ReadState();
bool Validate(const Parameters& values, std::string& error, bool checkFiles=true);
Result Apply(const Parameters& draft);
Result Connect(const Parameters& draft);
Result Start(const Parameters& draft);
Result Pause();
Result Move(int type, unsigned connectionId,int dx,int dy);
Result Load(const std::wstring& path);
Result Save(const std::wstring& path,const Parameters& draft);
std::vector<std::wstring> Profiles();
std::vector<std::wstring> Models();
}
