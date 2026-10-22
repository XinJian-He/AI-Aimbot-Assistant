#include "../common/obfuscated_text.h"
#include "ui_backend.h"
#include "native_json.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <limits>
#include <thread>
#include <mutex>
#include <windows.h>
extern "C" wchar_t g_proxyDir[MAX_PATH];
extern std::recursive_mutex g_deviceConfigMutex;
namespace NativeBackend {
namespace fs=std::filesystem;
std::wstring Directory(){
    if(g_proxyDir[0])return g_proxyDir;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
    return fs::path(path).parent_path().wstring()+L"\\";
}
Parameters ReadParameters(){
    std::vector<char> buffer(65536);
    if(!Aim_DumpConfigJson(buffer.data(),static_cast<int>(buffer.size())))throw std::runtime_error(AIM_TEXT_TEMP("读取后端参数失败"));
    const auto json=NativeJson::parse(buffer.data());
    if(json.type!=NativeJson::Value::Object)throw std::runtime_error(AIM_TEXT_TEMP("后端参数格式无效"));
    Parameters result;
    for(const auto& [key,value]:json.object){
        if(value.type==NativeJson::Value::Array){std::string cats;for(const auto& item:value.array){if(!cats.empty())cats+=',';cats+=item.text();}result[key]=cats;}
        else result[key]=value.type==NativeJson::Value::Bool?(value.scalar==AIM_TEXT_TEMP("true")?"1":"0"):value.text();
    }
    return result;
}
State ReadState(){State s;s.running=Aim_IsRunning()!=0;s.paused=Aim_IsPaused()!=0;s.connection=Aim_GetDeviceConnection();
    s.idle=Aim_IsProcessingIdle()!=0;s.captureFailed=Aim_IsCaptureFailed()!=0;s.moveMethod=Aim_GetMoveMethod();s.captureBackend=Aim_GetCaptureBackend();s.telemetry=Aim_GetTelemetry();
    s.connected=s.connection.state==2 && (s.moveMethod==0?Aim_IsKMBoxConnected():s.moveMethod==1?Aim_IsMAKCUConnected():s.moveMethod==2?Aim_IsDujiaReady():0)!=0;
    return s;
}
Snapshot ReadSnapshot(){
    Snapshot s;
    try{
        s.state=ReadState();
        s.state.resources=ReadResourceUsage();
        // Starting/stopping the model can hold this lock for seconds. Skip the
        // parameter refresh and retain the last accepted draft during that time.
        {std::unique_lock<std::recursive_mutex> lock(g_deviceConfigMutex,std::try_to_lock);
         if(lock.owns_lock())s.parameters=ReadParameters();}
        s.logs=GetRecentBackendLogs();
        s.profiles=Profiles();s.models=Models();
    }catch(const std::exception& e){s.error=e.what();}catch(...){s.error=AIM_TEXT_TEMP("后端状态读取失败");}
    return s;
}
Result CaptureDevices(unsigned generation){
    Result r;r.generation=generation;
    try{r.devicesJson=Aim_GetCaptureDevicesJson();auto v=NativeJson::parse(r.devicesJson);
        r.ok=v.type==NativeJson::Value::Array;r.message=r.ok?AIM_TEXT_TEMP("设备枚举完成"):v.at(AIM_TEXT_TEMP("error")).text();
    }catch(const std::exception& e){r.message=e.what();}catch(...){r.message=AIM_TEXT_TEMP("设备枚举失败");}return r;
}
Result CaptureModes(const std::string& device,unsigned generation){
    Result r;r.generation=generation;r.device=device;
    try{if(!Aim_IsProcessingIdle()){r.message=AIM_TEXT_TEMP("请先暂停处理，避免重复打开正在采集的设备");return r;}
        if(device.empty()){r.message=AIM_TEXT_TEMP("请先选择设备");return r;}
        r.modesJson=Aim_GetCaptureModesJson(device.c_str());auto v=NativeJson::parse(r.modesJson);
        r.ok=v.type==NativeJson::Value::Array;r.message=r.ok?AIM_TEXT_TEMP("格式枚举完成"):v.at(AIM_TEXT_TEMP("error")).text();
    }catch(const std::exception& e){r.message=e.what();}catch(...){r.message=AIM_TEXT_TEMP("格式枚举失败");}return r;
}
static bool Persist(){std::error_code ec;fs::create_directories(fs::path(Directory())/L"配置",ec);
    return !ec && Aim_SaveConfigW((fs::path(Directory())/L"配置"/AIM_TEXT_TEMP(L"aim_config.ini")).c_str())!=0;}
struct ParameterSpec {const char* key;char type;double lo,hi;size_t capacity;};
static const ParameterSpec specs[]={
#include "parameter_schema.inc"
};
static std::wstring ToWide(const std::string& text,UINT codepage=CP_UTF8){
    if(text.empty())return {};
    int n=MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(n<=0)throw std::runtime_error(AIM_TEXT_TEMP("无效的文本编码"));
    std::wstring out(n,L'\0');MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n);return out;
}
bool Validate(const Parameters& values,std::string& error,bool checkFiles){
    try {
        for(const auto& [key,value]:values){
            auto it=std::find_if(std::begin(specs),std::end(specs),[&](const auto& spec){return key==spec.key;});
            if(it==std::end(specs))throw std::runtime_error(key+AIM_TEXT_TEMP(": 未知参数"));
            auto fail=[&](const char* why){throw std::runtime_error(key+": "+why);};
            if(value.find('\0')!=std::string::npos||value.find_first_of(AIM_TEXT_TEMP("\r\n"))!=std::string::npos)fail(AIM_TEXT_TEMP("不允许换行或空字符"));
            if(it->type=='s'){
                if(value.size()>=it->capacity)fail(AIM_TEXT_TEMP("文本过长"));
                auto wide=ToWide(value);
                if(key==AIM_TEXT_TEMP("model_path")&&checkFiles&&!value.empty()){
                    std::error_code ec;if(!fs::is_regular_file(fs::path(wide),ec))fail(AIM_TEXT_TEMP("模型文件不存在"));
                }
                continue;
            }
            if(it->type=='c'){
                if(value.empty())continue;
                if(value.back()==',')fail(AIM_TEXT_TEMP("类别列表末尾不能为空"));
                std::istringstream input(value);std::string token;std::set<long> ids;
                while(std::getline(input,token,',')){
                    if(token.empty()||token.find_first_not_of(AIM_TEXT_TEMP("0123456789"))!=std::string::npos)fail(AIM_TEXT_TEMP("类别应为逗号分隔的非负整数"));
                    size_t used=0;long n=std::stol(token,&used);
                    if(n>std::numeric_limits<int>::max()||!ids.insert(n).second)fail(AIM_TEXT_TEMP("类别重复或超出范围"));
                }continue;
            }
            char* end=nullptr;double n=std::strtod(value.c_str(),&end);
            if(value.empty()||end!=value.c_str()+value.size()||!std::isfinite(n))fail(AIM_TEXT_TEMP("无效数值"));
            if(it->type!='f'&&(std::floor(n)!=n||n<std::numeric_limits<int>::min()||n>std::numeric_limits<int>::max()))fail(AIM_TEXT_TEMP("需要整数"));
            if(it->type=='f'&&std::abs(n)>std::numeric_limits<float>::max())fail(AIM_TEXT_TEMP("数值超出范围"));
            if((it->lo||it->hi)&&(n<it->lo||n>it->hi))fail(AIM_TEXT_TEMP("超出允许范围"));
            if(it->type=='b'&&n!=0&&n!=1)fail(AIM_TEXT_TEMP("布尔值应为 0 或 1"));
            if(key==AIM_TEXT_TEMP("makcu_scheme")&&n!=2)fail(AIM_TEXT_TEMP("只支持 Compatible SDK / negotiated 4M"));
            if(key==AIM_TEXT_TEMP("auto_fire_button")&&n!=VK_LBUTTON&&n!=VK_RBUTTON&&n!=VK_MBUTTON&&n!=VK_XBUTTON1&&n!=VK_XBUTTON2)fail(AIM_TEXT_TEMP("只能选择左键、右键、中键或侧键"));
            if((key==AIM_TEXT_TEMP("hotkey1")||key==AIM_TEXT_TEMP("hotkey2"))&&(n<1||n>255))fail(AIM_TEXT_TEMP("VK 应为 1 到 255"));
            if((key==AIM_TEXT_TEMP("conf")||key==AIM_TEXT_TEMP("vel_alpha"))&&(n<0||n>1))fail(AIM_TEXT_TEMP("应为 0 到 1"));
            if(key==AIM_TEXT_TEMP("aim_pct")&&(n<-100||n>100))fail(AIM_TEXT_TEMP("应为 -100 到 100"));
            if((key==AIM_TEXT_TEMP("cap_r")||key==AIM_TEXT_TEMP("aim_r"))&&(n<1||n>4096))fail(AIM_TEXT_TEMP("半径应为 1 到 4096"));
            if(key.rfind(AIM_TEXT_TEMP("fc_h"),0)==0&&(n<0||n>180))fail(AIM_TEXT_TEMP("色相应为 0 到 180"));
            if((key.rfind(AIM_TEXT_TEMP("fc_s"),0)==0||key.rfind(AIM_TEXT_TEMP("fc_v"),0)==0)&&(n<0||n>255))fail(AIM_TEXT_TEMP("色值应为 0 到 255"));
            if((key==AIM_TEXT_TEMP("max_capture_fps")||key==AIM_TEXT_TEMP("max_drop_frames")||key==AIM_TEXT_TEMP("max_compensate_frames")||key==AIM_TEXT_TEMP("min_lock_frames")||key==AIM_TEXT_TEMP("bez_min_d")||key==AIM_TEXT_TEMP("bez_max_d")||key==AIM_TEXT_TEMP("bez_dur")||key==AIM_TEXT_TEMP("bez_step")||key==AIM_TEXT_TEMP("nn_step"))&&n<0)fail(AIM_TEXT_TEMP("不能小于零"));
        }
        return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
Result Apply(const Parameters& draft){
    Result r; r.ok=true;
    std::unique_lock<std::recursive_mutex> configLock(g_deviceConfigMutex,std::try_to_lock);
    if(!configLock.owns_lock()){r.ok=false;r.message=AIM_TEXT_TEMP("后端正在加载或切换设备，请稍后再试");return r;}
    if(Aim_GetDeviceConnection().state==1){r.ok=false;r.message=AIM_TEXT_TEMP("设备正在连接，请稍后应用");return r;}
    if(!Validate(draft,r.message)){r.ok=false;return r;}
    auto merged=ReadParameters();for(const auto& item:draft)merged[item.first]=item.second;
    auto ordered=[&](const std::string& lo,const std::string& hi){return (!draft.count(lo)&&!draft.count(hi))||std::stod(merged.at(lo))<=std::stod(merged.at(hi));};
    if(!ordered(AIM_TEXT_TEMP("bez_min_d"),AIM_TEXT_TEMP("bez_max_d"))){r.ok=false;r.message=AIM_TEXT_TEMP("最小延迟不能大于最大延迟");return r;}
    for(const char* c:{"h","s","v"})for(int i=1;i<=2;++i){std::string prefix="fc_"+std::string(c)+std::to_string(i);
        if(!ordered(prefix+"lo",prefix+"hi")){r.ok=false;r.message=AIM_TEXT_TEMP("HSV 下限不能大于上限");return r;}}
    // Validate the full submission before any mutation, then read back accepted keys.
    for(const auto& [key,value]:draft){char error[2048]{};
        if(!Aim_ApplyParam(key.c_str(),value.c_str(),error,sizeof(error))){r.ok=false;r.message+=key+": "+(error[0]?error:AIM_TEXT_TEMP("应用失败"))+"\n";continue;}
        char actual[4096]{};if(Aim_GetParam(key.c_str(),actual,sizeof(actual)))r.confirmed[key]=actual;
        else{r.ok=false;r.message+=key+AIM_TEXT_TEMP(": 回读失败\n");}
    }
    r.persisted=Persist();
    if(r.ok)r.message=r.persisted?AIM_TEXT_TEMP("参数已应用并保存"):AIM_TEXT_TEMP("参数已应用，但默认配置保存失败");
    else if(!r.persisted)r.message+=AIM_TEXT_TEMP("默认配置保存失败");
    return r;
}
Result Connect(const Parameters& draft){
    auto r=Apply(draft);if(!r.ok)return r;
    auto pause=Pause();if(!pause.ok){r.ok=false;r.message=pause.message;return r;}
    r.connectionId=Aim_ConnectDevice();if(!r.connectionId){r.ok=false;r.message=AIM_TEXT_TEMP("设备连接请求未接受");return r;}
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(std::chrono::steady_clock::now()<until){auto c=Aim_GetDeviceConnection();
        if(c.requestId!=r.connectionId){r.ok=false;r.message=AIM_TEXT_TEMP("连接请求已被替换");return r;}
        if(c.state==2||c.state==3){r.ok=c.state==2;r.message=r.ok?AIM_TEXT_TEMP("设备连接成功"):std::string(AIM_TEXT_TEMP("设备连接失败："))+c.error;return r;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    r.ok=false;r.message=AIM_TEXT_TEMP("连接尚未完成，请查看设备状态；不会重复连接");return r;
}
Result Start(const Parameters& draft){
    auto paused=Pause();if(!paused.ok)return paused;
    auto r=Apply(draft);if(!r.ok)return r;
    const auto config=ReadParameters();
    if(config.at(AIM_TEXT_TEMP("cap_backend"))=="4"&&config.at(AIM_TEXT_TEMP("capture_device")).empty()){
        r.ok=false;r.message=AIM_TEXT_TEMP("请明确选择采集设备，避免误开内置摄像头");return r;
    }
    const auto baseline=Aim_GetCaptureFrameCount();
    Aim_Start();const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<until){
        if(Aim_IsRunning()&&!Aim_IsPaused()&&Aim_GetCaptureBackend()>=0&&Aim_GetCaptureFrameCount()>baseline){
            r.message=AIM_TEXT_TEMP("处理链路已启动，已收到采集画面");r.classesJson=Aim_GetModelClassesJson();return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    r.ok=false;
    r.message=Aim_IsCaptureFailed()?AIM_TEXT_TEMP("采集初始化失败，请查看日志中的设备或格式错误"):
        Aim_IsRunning()?AIM_TEXT_TEMP("已请求启动，但尚未收到采集画面；请检查设备占用和输入信号"):
        AIM_TEXT_TEMP("模型或处理链路未就绪，请查看后端日志");
    return r;
}
Result Pause(){
    Result r;
    if(Aim_IsProcessingIdle()){r.ok=true;r.message=AIM_TEXT_TEMP("处理链路已暂停");return r;}
    {
        std::unique_lock<std::recursive_mutex> lock(g_deviceConfigMutex,std::try_to_lock);
        if(!lock.owns_lock()){r.message=AIM_TEXT_TEMP("后端仍在加载或切换，请等待当前操作完成");return r;}
        Aim_Pause();
    }
    for(int i=0;i<120;++i){
        if(Aim_IsProcessingIdle()){r.ok=true;r.message=AIM_TEXT_TEMP("处理链路已暂停，采集设备已释放");return r;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    r.message=AIM_TEXT_TEMP("采集或推理尚未停止，未继续操作；请查看后端日志");return r;
}
Result Move(int type,unsigned id,int dx,int dy){Result r;char reason[2048]{};
    if(type==1){unsigned test=0;r.ok=Aim_TestMAKCUMove(2,id,dx,dy,&test,reason,sizeof(reason))==0;}
    else r.ok=Aim_TestMove(type,id,dx,dy,reason,sizeof(reason))==0;
    r.message=r.ok?AIM_TEXT_TEMP("移动指令已提交，请观察实际移动"):std::string(AIM_TEXT_TEMP("移动失败："))+reason;return r;
}
Result Load(const std::wstring& path){Result r;if(Aim_GetDeviceConnection().state==1){r.message=AIM_TEXT_TEMP("设备正在连接");return r;}
    std::ifstream file(fs::path(path),std::ios::binary);
    if(!file){r.message=AIM_TEXT_TEMP("无法打开配置文件");return r;}
    std::string content;char chunk[4096];
    while(file.read(chunk,sizeof(chunk))||file.gcount()){
        content.append(chunk,static_cast<size_t>(file.gcount()));
        if(content.size()>262144){r.message=AIM_TEXT_TEMP("配置文件过大");return r;}}
    if(file.bad()){r.message=AIM_TEXT_TEMP("配置读取失败");return r;}
    if(content.compare(0,3,AIM_TEXT_TEMP("\xef\xbb\xbf"))==0)content.erase(0,3);
    if(content.find('\0')!=std::string::npos){r.message=AIM_TEXT_TEMP("配置文件编码无效");return r;}
    try{ToWide(content);}catch(...){
        auto w=ToWide(content,CP_ACP);int n=WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),nullptr,0,nullptr,nullptr);
        content.resize(n);WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),content.data(),n,nullptr,nullptr);
    }
    Parameters current;
    {std::unique_lock<std::recursive_mutex> lock(g_deviceConfigMutex,std::try_to_lock);
     if(!lock.owns_lock()){r.message=AIM_TEXT_TEMP("后端仍在加载或切换，请稍后再加载配置");return r;}
     current=ReadParameters();}
    Parameters values;std::istringstream input(content);std::string line,oldCamera;bool legacy=false,modern=false;
    while(std::getline(input,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();auto eq=line.find('=');if(eq==std::string::npos)continue;
        auto key=line.substr(0,eq),value=line.substr(eq+1);
        if(key==AIM_TEXT_TEMP("image_source")){legacy=value=="1";continue;}if(key==AIM_TEXT_TEMP("camera_index")){oldCamera=value;continue;}
        if(key==AIM_TEXT_TEMP("auto_fire_hotkey"))key=AIM_TEXT_TEMP("auto_fire_button");
        if(!current.count(key))continue;
        if(key==AIM_TEXT_TEMP("capture_device")||key==AIM_TEXT_TEMP("capture_mode")||key==AIM_TEXT_TEMP("capture_udp_port")||key==AIM_TEXT_TEMP("capture_stream_port"))modern=true;
        if(key==AIM_TEXT_TEMP("makcu_scheme"))value="2";
        if(value==AIM_TEXT_TEMP("true")||value==AIM_TEXT_TEMP("TRUE"))value="1";else if(value==AIM_TEXT_TEMP("false")||value==AIM_TEXT_TEMP("FALSE"))value="0";
        if((key==AIM_TEXT_TEMP("hotkey1")||key==AIM_TEXT_TEMP("hotkey2")||key==AIM_TEXT_TEMP("auto_fire_button"))&&value.rfind("0x",0)==0){size_t used=0;int vk=std::stoi(value,&used,16);if(used!=value.size()){r.message=AIM_TEXT_TEMP("热键值无效");return r;}value=std::to_string(vk);}
        if(!values.emplace(key,value).second){r.message=AIM_TEXT_TEMP("配置键重复：")+key;return r;}
    }
    if(legacy&&!modern){values[AIM_TEXT_TEMP("cap_backend")]="4";if(!oldCamera.empty())values[AIM_TEXT_TEMP("capture_device")]=oldCamera;}
    if(values.empty()){r.message=AIM_TEXT_TEMP("文件不包含有效配置参数");return r;}
    if(!Validate(values,r.message))return r;
    auto paused=Pause();if(!paused.ok)return paused;
    for(auto it=values.begin();it!=values.end();)if(current[it->first]==it->second)it=values.erase(it);else ++it;
    r=Apply(values);
    if(r.ok){r.confirmed=ReadParameters();r.replaceDraft=true;r.message=r.persisted?AIM_TEXT_TEMP("配置已加载并保存为默认"):AIM_TEXT_TEMP("配置已加载，但默认配置保存失败");}
    return r;
}
Result Save(const std::wstring& path,const Parameters& draft){auto r=Apply(draft);if(!r.ok)return r;
    r.ok=Aim_SaveConfigW(path.c_str())!=0;r.message=r.ok?AIM_TEXT_TEMP("配置档案已保存"):AIM_TEXT_TEMP("配置档案保存失败");return r;}
static std::vector<std::wstring> List(const fs::path& dir,const wchar_t* extension){std::vector<std::wstring> out;std::error_code ec;
    for(fs::directory_iterator it(dir,ec),end;!ec&&it!=end;it.increment(ec))if(it->is_regular_file(ec)&&_wcsicmp(it->path().extension().c_str(),extension)==0)out.push_back(it->path().wstring());
    std::sort(out.begin(),out.end());return out;}
std::vector<std::wstring> Profiles(){return List(fs::path(Directory())/L"配置",AIM_TEXT_TEMP(L".ini"));}
std::vector<std::wstring> Models(){auto out=List(fs::path(Directory())/AIM_TEXT_TEMP(L"models"),AIM_TEXT_TEMP(L".onnx"));auto old=List(fs::path(Directory())/AIM_TEXT_TEMP(L"model"),AIM_TEXT_TEMP(L".onnx"));out.insert(out.end(),old.begin(),old.end());return out;}
}
