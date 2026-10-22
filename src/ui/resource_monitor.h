#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#pragma comment(lib, "pdh.lib")

namespace NativeBackend {
struct ResourceUsage {
    double memoryPercent=-1, memoryUsedGiB=0, memoryTotalGiB=0;
    double cpuPercent=-1, gpuPercent=-1;
};

// Invoked only by the background snapshot worker, at most once per second.
// GPU instances are per process: sum matching physical engines, then take
// the busiest engine. Summing unrelated engines would overstate utilization.
class ResourceMonitor {
    using Clock=std::chrono::steady_clock;
    PDH_HQUERY query=nullptr;
    PDH_HCOUNTER cpu=nullptr, gpu=nullptr;
    Clock::time_point sampled{}, retry{};
    bool primed=false;
    ResourceUsage cached;
    std::mutex mutex;
    static bool Valid(DWORD status){return status==PDH_CSTATUS_VALID_DATA||status==PDH_CSTATUS_NEW_DATA;}
    void Close(){if(query)PdhCloseQuery(query);query=nullptr;cpu=nullptr;gpu=nullptr;primed=false;}
    void Open(Clock::time_point now){
        Close();retry=now+std::chrono::seconds(30);
        if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS)return;
        if(PdhAddEnglishCounterW(query,L"\\Processor Information(_Total)\\% Processor Time",0,&cpu)!=ERROR_SUCCESS)cpu=nullptr;
        if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&gpu)!=ERROR_SUCCESS)gpu=nullptr;
        if(!cpu&&!gpu)Close();
    }
    double ReadGpu(){
        if(!gpu)return -1;
        DWORD bytes=0,count=0;
        auto status=PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE,&bytes,&count,nullptr);
        if(status!=PDH_MORE_DATA||!bytes||bytes>16*1024*1024)return -1;
        // Correct alignment for PDH_FMT_COUNTERVALUE_ITEM_W and trailing names.
        std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer((bytes+sizeof(PDH_FMT_COUNTERVALUE_ITEM_W)-1)/sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        status=PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE,&bytes,&count,buffer.data());
        if(status!=ERROR_SUCCESS)return -1;
        std::map<std::wstring,double> engines;
        for(DWORD i=0;i<count;++i){const auto& item=buffer[i];
            if(!item.szName||!Valid(item.FmtValue.CStatus)||!std::isfinite(item.FmtValue.doubleValue))continue;
            std::wstring name=item.szName;const auto physical=name.find(L"luid_");
            if(physical==std::wstring::npos)continue;
            engines[name.substr(physical)]+=std::max(0.0,item.FmtValue.doubleValue);
        }
        double peak=-1;for(const auto& item:engines)peak=std::max(peak,item.second);
        return peak<0?-1:std::clamp(peak,0.0,100.0);
    }
public:
    ~ResourceMonitor(){Close();}
    ResourceUsage Read(){
        std::lock_guard<std::mutex> lock(mutex);
        const auto now=Clock::now();
        if(sampled!=Clock::time_point{}&&now-sampled<std::chrono::seconds(1))return cached;
        sampled=now;cached={};
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        if(GlobalMemoryStatusEx(&memory)&&memory.ullTotalPhys){
            cached.memoryTotalGiB=memory.ullTotalPhys/1073741824.0;
            cached.memoryUsedGiB=(memory.ullTotalPhys-memory.ullAvailPhys)/1073741824.0;
            cached.memoryPercent=100.0*cached.memoryUsedGiB/cached.memoryTotalGiB;
        }
        if((!query||!cpu||!gpu)&&now>=retry)Open(now);
        if(!query)return cached;
        if(PdhCollectQueryData(query)!=ERROR_SUCCESS){Close();return cached;}
        if(!primed){primed=true;return cached;}
        if(cpu){PDH_FMT_COUNTERVALUE value{};
            if(PdhGetFormattedCounterValue(cpu,PDH_FMT_DOUBLE,nullptr,&value)==ERROR_SUCCESS&&Valid(value.CStatus)&&std::isfinite(value.doubleValue))
                cached.cpuPercent=std::clamp(value.doubleValue,0.0,100.0);
        }
        cached.gpuPercent=ReadGpu();return cached;
    }
};
inline ResourceUsage ReadResourceUsage(){static ResourceMonitor monitor;return monitor.Read();}
}
