#include "../common/obfuscated_text.h"
#define NOMINMAX
#include "app_ui.h"
#include "ui_backend.h"
#include "native_json.h"
#include <future>
#include <functional>
#include <chrono>
#include <filesystem>
#include <locale>
#include <limits>
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace AppUI
{
namespace
{
enum Kind
{
    Number,
    Integer,
    Boolean,
    Text,
    Choice
};
struct Field
{
    std::string key, label, hint;
    Kind kind;
    float value, lo, hi;
    std::string options;
    std::array<char, 4096> text{};
};
std::vector<Field> fields, applied;
int profileIndex = 0, page = 0, subtab = 0;
bool lightTheme = false, running = false, connected = false, motion = true;
float scale = 1, themeBlend = 0, pageFade = 1, navY = 114;
float transitionAge=1, transitionDirection=1;
int requestedSubtab=-1, transitionCard=0;
bool resetContentScroll=false;
std::array<float,7> tabLineX{}, tabLineWidth{}, navHover{};
std::array<bool,7> tabLineReady{};
std::array<float,5> resourceLevels{};
void startTransition(int from,int to){
    if(from==to)return;
    resetContentScroll=true;
    transitionDirection=to>from?1.f:-1.f;transitionAge=motion?0.f:1.f;
    pageFade=motion?0.f:1.f;
}
float transitionOffset(){float t=1-pageFade;return motion?transitionDirection*18*t*t:0;}
std::string toast;
float toastLife = 0;
std::deque<RecentLog::Entry> events;
RecentLog::Clock::time_point logClearTime{};
char search[160] = "";
bool followLog = true;
ImVec4 accent = ImVec4(.48f, .66f, .95f, 1);
ImFont *regular = nullptr;
ImTextureID humanTexture = 0;
ImTextureID avatarTexture = 0;
bool layoutFits = true;
ImGuiStyle baseStyle;
bool cardPaddingFits = true;
int curvePart = 0;
struct FpsSample
{
    double time;
    float value;
    bool gap;
};
struct FpsHistory
{
    std::deque<FpsSample> samples;
    double clock = 0;
} fpsHistory;
std::deque<FpsSample> frozenFps;
bool freezeFps = false;
double frozenTime = 0;
int fpsRange = 60;
struct Palette
{
    ImVec4 bg, side, card, hover, border, text, muted, track, select;
} p;
ImVec4 mix(ImVec4 a, ImVec4 b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}
ImU32 col(ImVec4 c, float alpha = 1)
{
    c.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}
ImVec2 at(ImVec2 v, float x, float y)
{
    return {v.x + x * scale, v.y + y * scale};
}
void text(ImVec2 pos, const char *s, float size, ImVec4 color)
{
    ImGui::GetWindowDrawList()->AddText(regular, (size + 1.5f) * scale, pos, col(color), s);
}
void notify(const std::string &s)
{
    toast = s;
    toastLife = 3;
    events.push_back({0,RecentLog::Clock::now(),s});
    RecentLog::Prune(events);
}
Field &f(const char *key)
{
    auto i = std::find_if(fields.begin(), fields.end(), [&](const Field &q) { return q.key == key; });
    IM_ASSERT(i != fields.end());
    return *i;
}
void add(const char *key, const char *label, Kind kind, float value = 0, float lo = 0, float hi = 1,
         const char *hint = "", const char *options = "", const char *str = "")
{
    Field q{key, label, hint, kind, value, lo, hi, options};
    strncpy_s(q.text.data(), q.text.size(), str, _TRUNCATE);
    fields.push_back(q);
}
void defaults()
{
    fields.clear();
    add(AIM_TEXT("proportion"), AIM_TEXT("比例增益"), Number, 25, .1f, 100, AIM_TEXT("响应强度"));
    add("kd", AIM_TEXT("微分抑制"), Number, 0, 0, 100, AIM_TEXT("抑制过冲与抖动"));
    add(AIM_TEXT("rate"), AIM_TEXT("移动速率"), Number, .3f, 0, 3);
    add(AIM_TEXT("predict_x"), AIM_TEXT("X 轴预测"), Number, 1, 0, 3);
    add(AIM_TEXT("predict_y"), AIM_TEXT("Y 轴预测"), Number, 0, 0, 3);
    add(AIM_TEXT("vel_alpha"), AIM_TEXT("速度平滑"), Number, .35f, 0, 1);
    add(AIM_TEXT("smooth"), AIM_TEXT("输出平滑"), Number, 9900, 0, 10000);
    add(AIM_TEXT("out_max"), AIM_TEXT("最大输出"), Number, 0, 0, 500, AIM_TEXT("0 表示不限制"));
    add("dz", AIM_TEXT("死区距离"), Number, .5f, 0, 5);
    add(AIM_TEXT("max_drop_frames"), AIM_TEXT("最大丢帧"), Integer, 3, 0, 30);
    add(AIM_TEXT("max_compensate_frames"), AIM_TEXT("最大补偿帧"), Integer, 0, 0, 30);
    add(AIM_TEXT("min_lock_frames"), AIM_TEXT("最小锁定帧"), Integer, 1, 0, 30);
    add(AIM_TEXT("same_dist"), AIM_TEXT("同目标距离²"), Number, 2500, 100, 10000);
    add(AIM_TEXT("conf"), AIM_TEXT("置信度阈值"), Number, .25f, .05f, .95f, AIM_TEXT("过滤低置信度目标"));
    add(AIM_TEXT("auto_fire"), AIM_TEXT("自动开火"), Boolean, 0);
    add(AIM_TEXT("auto_fire_button"), AIM_TEXT("开火输出键"), Integer, 1, 1, 6);
    add(AIM_TEXT("continuous_lock"), AIM_TEXT("持续锁定"), Boolean, 0, 0, 1, AIM_TEXT("保持目标跟踪"));
    add(AIM_TEXT("fire_mode"), AIM_TEXT("开火判定"), Choice, 1, 0, 1, "", AIM_TEXT("中心点半径|识别框比例"));
    add(AIM_TEXT("fire_r"), AIM_TEXT("开火半径"), Number, 3, .5f, 30);
    add(AIM_TEXT("fire_box_ratio"), AIM_TEXT("开火框比例"), Number, .8f, .1f, 1);
    add(AIM_TEXT("aim_pct"), AIM_TEXT("瞄点偏移 / %"), Number, 0, -100, 100);
    add(AIM_TEXT("y_off"), AIM_TEXT("Y 偏移 / 压枪"), Integer, 0, -50, 50);
    add(AIM_TEXT("aim_r"), AIM_TEXT("瞄准范围 / px"), Integer, 160, 1, 640);
    add(AIM_TEXT("categories"), AIM_TEXT("类别优先级 ID"), Text, 0, 0, 1, AIM_TEXT("从左到右依次优先"), "", AIM_TEXT("0,1,2,3"));
    add(AIM_TEXT("hotkey1_enabled"), AIM_TEXT("启用热键 1"), Boolean, 1);
    add(AIM_TEXT("hotkey2_enabled"), AIM_TEXT("启用热键 2"), Boolean, 0);
    add(AIM_TEXT("hotkey1"), AIM_TEXT("自瞄热键 1"), Integer, 1, 1, 255);
    add(AIM_TEXT("hotkey2"), AIM_TEXT("自瞄热键 2"), Integer, 2, 1, 255);
    add("cs", AIM_TEXT("急停保护"), Boolean, 0, 0, 1, AIM_TEXT("激活时停止反向移动"));
    add(AIM_TEXT("cs_backend"), AIM_TEXT("急停注入后端"), Choice, 2, 0, 3, "", AIM_TEXT("自动|KMBox|SendInput|MAKCU"));
    add(AIM_TEXT("bez_enabled"), AIM_TEXT("贝塞尔曲线"), Boolean, 0);
    add(AIM_TEXT("nn_enabled"), AIM_TEXT("神经网络曲线"), Boolean, 0);
    add(AIM_TEXT("bez_dist"), AIM_TEXT("触发距离"), Number, 50, 0, 300);
    add(AIM_TEXT("bez_dev"), AIM_TEXT("曲线偏移"), Number, .28f, 0, 1);
    add(AIM_TEXT("bez_min_d"), AIM_TEXT("最小延迟 / ms"), Integer, 2, 0, 20);
    add(AIM_TEXT("bez_max_d"), AIM_TEXT("最大延迟 / ms"), Integer, 5, 0, 30);
    add(AIM_TEXT("bez_dur"), AIM_TEXT("曲线时长 / ms"), Integer, 10, 0, 50);
    add(AIM_TEXT("bez_step"), AIM_TEXT("每步间隔 / ms"), Integer, 3, 0, 20);
    add(AIM_TEXT("bez_direct"), AIM_TEXT("直达阈值 / px"), Integer, 5, 0, 100);
    add(AIM_TEXT("nn_dist"), AIM_TEXT("启用距离 / px"), Number, 15, 0, 300);
    add(AIM_TEXT("nn_step"), AIM_TEXT("每步间隔 / ms"), Integer, 2, 0, 20);
    add(AIM_TEXT("findcolor"), AIM_TEXT("找色辅助"), Boolean, 0, 0, 1, AIM_TEXT("HSV 双区颜色筛选"));
    add(AIM_TEXT("fc_radius"), AIM_TEXT("区域半径 / px"), Integer, 40, 10, 300);
    add(AIM_TEXT("fc_aim_scale"), AIM_TEXT("瞄准缩放"), Number, .7f, 0, 2);
    add(AIM_TEXT("fc_min_area"), AIM_TEXT("最小面积"), Integer, 2, 0, 500);
    for (int i = 1; i <= 2; i++)
        for (int j = 0; j < 6; j++)
        {
            const char *keys[] = {"h", "h", "s", "s", "v", "v"};
            const char *labels[] = {AIM_TEXT("H 色相下限"),   AIM_TEXT("H 色相上限"), AIM_TEXT("S 饱和度下限"),
                                    AIM_TEXT("S 饱和度上限"), AIM_TEXT("V 明度下限"), AIM_TEXT("V 明度上限")};
            float values[] = {i == 1 ? 0.f : 177.f, i == 1 ? 3.f : 180.f, 100, 255, 120, 255};
            auto k = "fc_" + std::string(keys[j]) + std::to_string(i) + (j % 2 ? "hi" : "lo");
            add(k.c_str(), labels[j], Number, values[j], 0, j < 2 ? 180.f : 255.f);
        }
    add(AIM_TEXT("device_type"), AIM_TEXT("执行设备"), Choice, 1, 0, 2, "", AIM_TEXT("KMBox (UDP)|MAKCU (串口)|独家驱动"));
    add(AIM_TEXT("km_ip"), AIM_TEXT("IP 地址"), Text, 0, 0, 1, "", "", AIM_TEXT("192.168.2.188"));
    add(AIM_TEXT("km_port"), "端口", Text, 0, 0, 1, "", "", AIM_TEXT("1000"));
    add(AIM_TEXT("km_mac"), AIM_TEXT("UUID / MAC"), Text, 0, 0, 1, "", "", "");
    add(AIM_TEXT("makcu_port"), "串口", Text, 0, 0, 1, AIM_TEXT("AUTO 可自动选择"), "", AIM_TEXT("COM3"));
    add(AIM_TEXT("makcu_scheme"), AIM_TEXT("固定连接策略"), Integer, 2, 2, 2);
    add(AIM_TEXT("makcu_baud"), AIM_TEXT("兼容波特率"), Integer, 4000000, 4000000, 4000000);
    add(AIM_TEXT("cap_backend"), AIM_TEXT("采集模式"), Choice, 1, 0, 5, "", AIM_TEXT("GDI|DXGI|WGC|UDP|采集卡|串流 (TCP)"));
    add(AIM_TEXT("capture_udp_port"), AIM_TEXT("UDP 接收端口"), Integer, 7788, 1, 65535);
    add(AIM_TEXT("capture_stream_port"), AIM_TEXT("串流接收端口"), Integer, 9339, 1, 65535);
    add(AIM_TEXT("capture_device"), AIM_TEXT("采集设备"), Text);
    add(AIM_TEXT("capture_mode"), AIM_TEXT("采集格式"), Text, 0, 0, 1, AIM_TEXT("留空时自动协商"));
    add(AIM_TEXT("cap_r"), AIM_TEXT("采集半径 / px"), Integer, 160, 80, 640);
    add(AIM_TEXT("max_capture_fps"), AIM_TEXT("帧率上限"), Integer, 0, 0, 1000, AIM_TEXT("0 表示不限制"));
    add(AIM_TEXT("model_path"), AIM_TEXT("模型文件"), Text, 0, 0, 1, AIM_TEXT("选择 .onnx 模型"), "", AIM_TEXT("未选择模型"));
    add(AIM_TEXT("inference_engine"), AIM_TEXT("推理引擎"), Choice, 1, 0, 1, "", AIM_TEXT("兼容模式|ONNX Runtime"));
    add(AIM_TEXT("onnx_device"), AIM_TEXT("推理执行后端"), Choice, 1, 0, 3, "", AIM_TEXT("自动|DirectML|CUDA|CPU"));
    add(AIM_TEXT("preview_win"), AIM_TEXT("实时预览"), Boolean, 0, 0, 1, AIM_TEXT("显示采集画面预览"));
}
std::vector<std::string> options(const std::string &s)
{
    std::stringstream stream(s);
    std::string item;
    std::vector<std::string> result;
    while (std::getline(stream, item, '|'))
        result.push_back(item);
    return result;
}
NativeBackend::State backendState;
std::future<NativeBackend::Result> pending;
std::future<NativeBackend::Snapshot> snapshotPending;
std::future<NativeBackend::Result> capturePending;
unsigned captureGeneration=0, parameterRevision=0;
bool captureRequested=false, modesRequested=false;
double pendingSince=0, captureSince=0;
std::string captureStatus=AIM_TEXT("请选择采集设备");
std::string modesDevice, observedCaptureDevice;
int observedCaptureBackend=-1;
bool snapshotFresh=false;
double lastSnapshot=0;
bool backendLoaded = false;
std::vector<std::wstring> profileFiles, modelFiles;
std::vector<std::pair<std::string,std::string>> captureDevices, captureModes, modelClasses;
std::string modelClassesPath;
std::uint64_t lastBackendLogId=0;
double nextBackendPoll = 0;
double backendClock(){using Clock=std::chrono::steady_clock;static const auto origin=Clock::now();return std::chrono::duration<double>(Clock::now()-origin).count();}
std::wstring wide(const std::string&);
std::string utf8(const std::wstring& s) {
    if(s.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr);return out;
}
bool busy(){return pending.valid();}
bool captureBusy(){return capturePending.valid();}
bool actionsBusy(){return busy() || captureBusy();}
std::string parameterValue(const Field& q){
    if(q.kind==Text)return q.text.data();
    if(q.kind==Boolean)return q.value!=0?"1":"0";
    if(q.kind==Integer||q.kind==Choice)return std::to_string(static_cast<long long>(q.value));
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9)<<q.value;return out.str();
}
bool sameField(const Field& a,const Field& b){return a.value==b.value&&std::strcmp(a.text.data(),b.text.data())==0;}
bool deviceField(const std::string& key){return key==AIM_TEXT("device_type")||key==AIM_TEXT("km_ip")||key==AIM_TEXT("km_port")||key==AIM_TEXT("km_mac")||key==AIM_TEXT("makcu_port");}
NativeBackend::Parameters draft(bool deviceOnly=false){NativeBackend::Parameters result;
    for(size_t i=0;i<fields.size();++i)if((!deviceOnly||deviceField(fields[i].key))&&!sameField(fields[i],applied[i]))result[fields[i].key]=parameterValue(fields[i]);return result;
}
void readField(Field& q,const std::string& value){
    if(value.find('\0')!=std::string::npos)throw std::runtime_error(q.label+AIM_TEXT("包含空字符"));
    if(q.kind==Text){if(value.size()>=q.text.size())throw std::runtime_error(q.label+"过长");q.text.fill(0);strcpy_s(q.text.data(),q.text.size(),value.c_str());return;}
    char* end=nullptr;float n=std::strtof(value.c_str(),&end);
    if(!end||*end||end==value.c_str()||!std::isfinite(n))throw std::runtime_error(q.label+AIM_TEXT("返回值无效"));
    if(q.kind!=Number && (n < static_cast<double>(std::numeric_limits<int>::min()) || n > static_cast<double>(std::numeric_limits<int>::max())))throw std::runtime_error(q.label+AIM_TEXT("整数超出范围"));
    q.value=n;
}
void acceptConfirmed(const NativeBackend::Parameters& values,bool replaceDraft){
    for(size_t i=0;i<fields.size();++i){auto found=values.find(fields[i].key);if(found==values.end())continue;
        readField(applied[i],found->second);if(replaceDraft)readField(fields[i],found->second);
    }
}
void submit(std::function<NativeBackend::Result()> action){
    if(actionsBusy()||!backendLoaded)return;
    ++parameterRevision;pendingSince=backendClock();
    pending=std::async(std::launch::async,[action=std::move(action)]{
        try{return action();}catch(const std::exception& e){NativeBackend::Result r;r.message=e.what();return r;}
        catch(...){NativeBackend::Result r;r.message=AIM_TEXT("操作失败，结果未确认，请勿自动重复操作");return r;}
    });
}
void applyDraft(){auto values=draft();submit([values]{return NativeBackend::Apply(values);});}
void requestStart(){
    if((int)f(AIM_TEXT("cap_backend")).value==4 && !f(AIM_TEXT("capture_device")).text[0]){
        notify(AIM_TEXT("请先选择采集设备，避免自动打开内置摄像头"));return;
    }
    auto values=draft();submit([values]{return NativeBackend::Start(values);});
}
void requestPause(){submit([]{return NativeBackend::Pause();});}
void requestConnect(){auto values=draft(true);submit([values]{return NativeBackend::Connect(values);});}
bool canMove(){return backendLoaded&&snapshotFresh&&!actionsBusy()&&backendState.idle&&backendState.connected&&backendState.connection.deviceType==static_cast<int>(f(AIM_TEXT("device_type")).value)&&draft(true).empty();}
void requestMove(int dx,int dy){
    if(!canMove()){notify(AIM_TEXT("请先应用设备参数、连接设备并暂停处理"));return;}
    int type=static_cast<int>(f(AIM_TEXT("device_type")).value);unsigned id=backendState.connection.requestId;
    submit([=]{return NativeBackend::Move(type,id,dx,dy);});
}
void parseList(const std::string& json,const char* id,const char* label,std::vector<std::pair<std::string,std::string>>& out){
    if(json.empty())return;auto value=NativeJson::parse(json);
    if(value.type!=NativeJson::Value::Array)throw std::runtime_error(value.at(AIM_TEXT("error")).text().empty()?AIM_TEXT("元数据格式无效"):value.at(AIM_TEXT("error")).text());
    out.clear();for(const auto& item:value.array)out.emplace_back(item.at(id).text(),item.at(label).text());
}
void refreshCapture(){
    if(actionsBusy())return;
    captureRequested=true; modesRequested=false; ++captureGeneration;
    captureStatus=AIM_TEXT("正在读取设备…");
    captureSince=backendClock();
    const auto generation=captureGeneration;
    capturePending=std::async(std::launch::async,[generation]{return NativeBackend::CaptureDevices(generation);});
}
void selectCaptureDevice(){
    ++captureGeneration;captureModes.clear();f(AIM_TEXT("capture_mode")).text.fill(0);
    modesDevice=f(AIM_TEXT("capture_device")).text.data();observedCaptureDevice=modesDevice;modesRequested=!modesDevice.empty();
    captureStatus=modesRequested?AIM_TEXT("等待读取格式…"):AIM_TEXT("请选择采集设备");
}
void refreshCaptureModes(){
    if(actionsBusy() || !backendState.idle || !snapshotFresh){captureStatus=AIM_TEXT("请先暂停处理，再读取格式");return;}
    modesDevice=f(AIM_TEXT("capture_device")).text.data();
    if(modesDevice.empty()){captureStatus=AIM_TEXT("请先选择采集设备");return;}
    ++captureGeneration;modesRequested=true;
}
void refreshClasses(){submit([]{NativeBackend::Result r;r.classesJson=Aim_GetModelClassesJson();r.ok=true;r.message=AIM_TEXT("模型类别已刷新");return r;});}
bool metadataCombo(const char* key,const std::vector<std::pair<std::string,std::string>>& entries){
    auto& field=f(key);ImGui::PushID(key);bool changed=false;
    const bool device=strcmp(key,AIM_TEXT("capture_device"))==0;
    const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return item.first==field.text.data();});
    std::string label=found!=entries.end()?found->second:field.text[0]?AIM_TEXT("已保存的选择（尚未枚举）"):device?AIM_TEXT("请选择设备"):AIM_TEXT("自动协商格式");
    ImGui::TextUnformatted(field.label.c_str());ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo(AIM_TEXT("##selection"),label.c_str())){
        if(ImGui::Selectable(device?AIM_TEXT("请选择设备"):AIM_TEXT("自动协商格式"),!field.text[0])){field.text.fill(0);changed=true;}
        for(const auto& [id,name]:entries){
            ImGui::PushID(id.c_str());
            if(ImGui::Selectable(name.c_str(),id==field.text.data()) && id.size()<field.text.size()){
                field.text.fill(0);strcpy_s(field.text.data(),field.text.size(),id.c_str());changed=true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()&&field.text[0])ImGui::SetTooltip("%s",field.text.data());
    ImGui::PopID();return changed;
}
void pollCapture(){
    if(capturePending.valid()&&capturePending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
        auto r=capturePending.get();
        if(r.generation==captureGeneration){
            try {
                if(!r.ok)throw std::runtime_error(r.message);
                if(!r.devicesJson.empty()){
                    parseList(r.devicesJson,"id",AIM_TEXT("name"),captureDevices);
                    captureStatus=captureDevices.empty()?AIM_TEXT("未发现视频采集设备，请检查 USB 连接和设备驱动"):std::to_string(captureDevices.size())+AIM_TEXT(" 个可选设备");
                    modesDevice=f(AIM_TEXT("capture_device")).text.data();
                    modesRequested=!modesDevice.empty()&&!running;
                }
                if(!r.modesJson.empty()&&r.device==f(AIM_TEXT("capture_device")).text.data()){
                    parseList(r.modesJson,"key",AIM_TEXT("label"),captureModes);
                    captureStatus=captureModes.empty()?AIM_TEXT("设备未提供可用格式，请检查占用和驱动"):std::to_string(captureModes.size())+AIM_TEXT(" 种可用格式");
                }
            }catch(const std::exception& e){captureStatus=e.what();notify(captureStatus);}
        }
    }
    if(capturePending.valid()&&backendClock()-captureSince>8)
        captureStatus=AIM_TEXT("设备驱动尚未返回（超过 8 秒），界面仍可操作；请检查其他程序是否占用设备");
    if(modesRequested&&captureRequested&&!captureDevices.empty()&&!actionsBusy()&&snapshotFresh&&backendState.idle&&(int)f(AIM_TEXT("cap_backend")).value==4){
        modesRequested=false;const auto generation=captureGeneration;const auto device=modesDevice;
        captureStatus=AIM_TEXT("正在读取所选设备格式…");captureSince=backendClock();
        capturePending=std::async(std::launch::async,[generation,device]{return NativeBackend::CaptureModes(device,generation);});
    }
}
void pollBackend(bool visible=true){
    RecentLog::Prune(events);
    if(pending.valid()&&pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
        auto r=pending.get();++parameterRevision;
        try{
            acceptConfirmed(r.confirmed,true);
            if(!r.classesJson.empty()){auto v=NativeJson::parse(r.classesJson);modelClassesPath=v.at(AIM_TEXT("path")).text();modelClasses.clear();for(auto& item:v.at(AIM_TEXT("classes")).array)modelClasses.emplace_back(item.at("id").text(),item.at(AIM_TEXT("name")).text());}
        }catch(const std::exception& e){r.message=e.what();}
        notify(r.message);nextBackendPoll=0;
    }
    if(snapshotPending.valid()&&snapshotPending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
        auto s=snapshotPending.get();
        if(!s.error.empty()){notify(s.error);nextBackendPoll=backendClock()+2;}
        else {
            backendState=s.state;running=s.state.running&&!s.state.paused;connected=s.state.connected;
            snapshotFresh=true;lastSnapshot=backendClock();
            if(!s.parameters.empty() && !busy() && s.revision==parameterRevision){
                for(size_t i=0;i<fields.size();++i)if(!backendLoaded||sameField(fields[i],applied[i])){
                    auto it=s.parameters.find(fields[i].key);if(it!=s.parameters.end()){readField(fields[i],it->second);applied[i]=fields[i];}
                }
                backendLoaded=true;
            }
            profileFiles=std::move(s.profiles);modelFiles=std::move(s.models);
            for(const auto& entry:s.logs)if(entry.id>lastBackendLogId){
                lastBackendLogId=entry.id;
                if(entry.created>logClearTime)events.push_back(entry);
            }
            std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.created<b.created;});
            RecentLog::Prune(events);
            fpsHistory.clock=backendClock();fpsHistory.samples.push_back({fpsHistory.clock,running?backendState.telemetry.fps:0,!running});
            while(!fpsHistory.samples.empty()&&fpsHistory.samples.front().time<fpsHistory.clock-130)fpsHistory.samples.pop_front();
        }
    }
    if(snapshotFresh&&backendClock()-lastSnapshot>2)snapshotFresh=false;
    if(!snapshotPending.valid()&&backendClock()>=nextBackendPoll){
        // Paused/hidden panels do not need four directory scans and snapshot
        // workers per second. Running telemetry keeps its original cadence.
        nextBackendPoll=backendClock()+(visible && (running || actionsBusy()) ? .25 : 1.0);
        // Every potentially blocking backend read stays off the Win32 render thread.
        const auto revision=parameterRevision;
        snapshotPending=std::async(std::launch::async,[revision]{auto s=NativeBackend::ReadSnapshot();s.revision=revision;return s;});
    }
    if(modelClassesPath!=f(AIM_TEXT("model_path")).text.data())modelClasses.clear();
    const std::string selected=f(AIM_TEXT("capture_device")).text.data();
    const int captureType=(int)f(AIM_TEXT("cap_backend")).value;
    if(selected!=observedCaptureDevice || captureType!=observedCaptureBackend){
        ++captureGeneration;captureModes.clear();modesDevice=selected;
        modesRequested=captureType==4&&!selected.empty();
        observedCaptureDevice=selected;observedCaptureBackend=captureType;
        // A late device-list response invalidated by this edit will be retried.
        if(captureDevices.empty())captureRequested=false;
    }
    pollCapture();
}

void palette()
{
    float target = lightTheme ? 1.f : 0.f;
    themeBlend += (target - themeBlend) * (motion ? 1 - std::exp(-ImGui::GetIO().DeltaTime * 12) : 1);
    auto c = [&](ImVec4 a, ImVec4 b) { return mix(a, b, themeBlend); };
    p.bg = c({.074f, .09f, .12f, 1}, {.935f, .955f, .976f, 1});
    p.side = c({.064f, .078f, .105f, 1}, {.90f, .93f, .962f, 1});
    p.card = c({.113f, .133f, .17f, 1}, {.981f, .989f, 1, 1});
    p.hover = c({.15f, .18f, .23f, 1}, {.88f, .93f, .985f, 1});
    p.border = c({.20f, .235f, .29f, .48f}, {.72f, .78f, .85f, .4f});
    p.text = c({.91f, .94f, .98f, 1}, {.15f, .205f, .28f, 1});
    p.muted = c({.50f, .56f, .66f, 1}, {.43f, .49f, .57f, 1});
    p.track = c({.22f, .26f, .32f, 1}, {.82f, .86f, .91f, 1});
    p.select = mix(p.card, accent, .24f - .07f * themeBlend);
    auto &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Text] = p.text;
    st.Colors[ImGuiCol_TextDisabled] = p.muted;
    st.Colors[ImGuiCol_WindowBg] = p.bg;
    st.Colors[ImGuiCol_ChildBg] = p.card;
    st.Colors[ImGuiCol_PopupBg] = p.card;
    st.Colors[ImGuiCol_Border] = p.border;
    st.Colors[ImGuiCol_FrameBg] = p.bg;
    st.Colors[ImGuiCol_FrameBgHovered] = p.hover;
    st.Colors[ImGuiCol_FrameBgActive] = p.select;
    st.Colors[ImGuiCol_Button] = p.select;
    st.Colors[ImGuiCol_ButtonHovered] = p.hover;
    st.Colors[ImGuiCol_ButtonActive] = accent;
    st.Colors[ImGuiCol_Header] = p.select;
    st.Colors[ImGuiCol_HeaderHovered] = p.hover;
    st.Colors[ImGuiCol_HeaderActive] = p.select;
    st.Colors[ImGuiCol_CheckMark] = accent;
    st.Colors[ImGuiCol_SliderGrab] = accent;
    st.Colors[ImGuiCol_SliderGrabActive] = accent;
    st.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    st.Colors[ImGuiCol_ScrollbarGrab] = p.track;
    st.Colors[ImGuiCol_Separator] = p.border;
}
void icon(ImVec2 center, int type, ImVec4 color, float r = 8)
{
    auto d = ImGui::GetWindowDrawList();
    float x = center.x, y = center.y, s = r * scale;
    ImU32 c = col(color);
    float w = 1.5f * scale;
    if (type == 0)
    {
        d->AddLine({x - s, y}, {x, y - s}, c, w);
        d->AddLine({x, y - s}, {x + s, y}, c, w);
        d->AddRect({x - s * .65f, y - s * .1f}, {x + s * .65f, y + s}, c, 1, 0, w);
    }
    else if (type == 1)
    {
        d->AddCircle(center, s * .75f, c, 24, w);
        d->AddCircle(center, s * .22f, c, 16, w);
        for (int i = 0; i < 4; i++)
        {
            float a = i * 1.570796f;
            d->AddLine({x + cosf(a) * s * .6f, y + sinf(a) * s * .6f}, {x + cosf(a) * s * 1.2f, y + sinf(a) * s * 1.2f},
                       c, w);
        }
    }
    else if (type == 2)
    {
        for (int i = 0; i < 3; i++)
        {
            float yy = y + (i - 1) * s * .7f;
            d->AddLine({x - s, yy}, {x + s, yy}, c, w);
            d->AddCircleFilled({x + (i == 1 ? .4f : -.35f) * s, yy}, 2.4f * scale, c);
        }
    }
    else if (type == 3)
    {
        for (int i = 0; i < 3; i++)
        {
            float a = i * 2.094f - 1.57f;
            d->AddCircle({x + cosf(a) * s * .5f, y + sinf(a) * s * .5f}, s * .55f, c, 20, w);
        }
    }
    else if (type == 4)
    {
        d->AddRect({x - s, y - s * .7f}, {x + s, y + s * .55f}, c, 2 * scale, 0, w);
        d->AddLine({x, y + s * .55f}, {x, y + s}, c, w);
        d->AddLine({x - s * .5f, y + s}, {x + s * .5f, y + s}, c, w);
    }
    else if (type == 5)
    {
        d->AddRect({x - s, y - s * .6f}, {x + s, y + s}, c, 2 * scale, 0, w);
        d->AddLine({x - s, y - s * .6f}, {x - s * .35f, y - s}, c, w);
        d->AddLine({x - s * .35f, y - s}, {x + s * .15f, y - s * .6f}, c, w);
    }
    else
    {
        for (int i = 0; i < 3; i++)
            d->AddLine({x - s, y + (i - 1) * s * .65f}, {x + s * (i == 2 ? .4f : 1), y + (i - 1) * s * .65f}, c, w);
    }
}
bool toggle(const char *id, bool &value)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(id, {36 * scale, 21 * scale});
    if (hit)
        value = !value;
    auto storage = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID(id);
    float v = storage->GetFloat(key, value ? 1.f : 0.f);
    v += ((value ? 1.f : 0.f) - v) * (motion ? 1 - std::exp(-ImGui::GetIO().DeltaTime * 18) : 1);
    storage->SetFloat(key, v);
    auto d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, at(a, 36, 21), col(mix(p.track, accent, v)), 11 * scale);
    d->AddCircleFilled(at(a, 10.5f + 15 * v, 10.5f), 7.1f * scale, col({.97f, .98f, 1, 1}));
    return hit;
}
void section(const char *title, const char *subtitle = nullptr)
{
    ImGui::Spacing();
    ImGui::TextUnformatted(title);
    if (subtitle)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(AIM_TEXT("  %s"), subtitle);
    }
    ImGui::Dummy({0, 5 * scale});
}
void beginCard(const char *id)
{
    const float reveal=motion?std::clamp((transitionAge-std::min(transitionCard++,4)*.025f)/.26f,0.f,1.f):1.f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,ImGui::GetStyle().Alpha*(.72f+.28f*reveal));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * scale, 8 * scale});
    ImGui::BeginChild(id, {0, 0},
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_Borders |
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 inset = ImGui::GetCursorScreenPos(), origin = ImGui::GetWindowPos();
    cardPaddingFits &= inset.x - origin.x >= 15 * scale && inset.y - origin.y >= 7 * scale;
}
void endCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 5 * scale});
}
bool row(const char *key)
{
    auto &q = f(key);
    ImGui::PushID(key);
    auto a = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float h = (q.hint.empty() ? 39.f : 48.f) * scale;
    float labelY = q.hint.empty() ? (h / scale - 15.f) * .5f : 5.f;
    float control = std::min(236.f * scale, width * .48f);
    auto draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(a, {a.x + width - control - 16 * scale, a.y + h}, true);
    text(at(a, 0, labelY), q.label.c_str(), 13.5f, p.text);
    if (!q.hint.empty())
        text(at(a, 0, 27), q.hint.c_str(), 10.5f, p.muted);
    draw->PopClipRect();
    ImGui::SetCursorScreenPos({a.x + width - control, a.y + (h - ImGui::GetFrameHeight()) / 2});
    ImGui::SetNextItemWidth(control);
    bool change = false;
    if (q.kind == Boolean)
    {
        ImGui::SetCursorScreenPos({a.x + width - 36 * scale, a.y + (h - 21 * scale) / 2});
        bool v = q.value != 0;
        change = toggle(AIM_TEXT("##toggle"), v);
        q.value = v ? 1.f : 0.f;
    }
    else if (q.kind == Text)
        change = ImGui::InputText(AIM_TEXT("##value"), q.text.data(), q.text.size());
    else if (q.key==AIM_TEXT("hotkey1") || q.key==AIM_TEXT("hotkey2") || q.key==AIM_TEXT("auto_fire_button"))
    {
        // Store Windows VK values, not combo indices (VK 3 is not middle mouse).
        constexpr int values[]={VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
        const char* labels[]={"左键","右键","中键",AIM_TEXT("侧键 1"),AIM_TEXT("侧键 2")};
        const int current=static_cast<int>(q.value);int selected=-1;
        for(int i=0;i<5;++i)if(values[i]==current)selected=i;
        const std::string legacy=AIM_TEXT("旧键位 VK ")+std::to_string(current)+AIM_TEXT("（可重选）");
        if(ImGui::BeginCombo(AIM_TEXT("##value"),selected>=0?labels[selected]:legacy.c_str())){
            for(int i=0;i<5;++i){
                if(ImGui::Selectable(labels[i],i==selected)){q.value=static_cast<float>(values[i]);change=true;}
                if(i==selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    else if (q.kind == Choice)
    {
        auto list = options(q.options);
        int v = std::clamp((int)q.value, 0, (int)list.size() - 1);
        if (ImGui::BeginCombo(AIM_TEXT("##value"), list[v].c_str()))
        {
            for (int i = 0; i < (int)list.size(); i++)
                if (ImGui::Selectable(list[i].c_str(), i == v))
                {
                    q.value = (float)i;
                    change = true;
                }
            ImGui::EndCombo();
        }
    }
    else
    {
        float sliderWidth = control - 86 * scale;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        auto d = ImGui::GetWindowDrawList();
        bool hovered = ImGui::IsMouseHoveringRect(a, {a.x + width, a.y + h});
        auto storage = ImGui::GetStateStorage();
        ImGuiID revealKey = ImGui::GetID(AIM_TEXT("reveal"));
        bool engaged = storage->GetBool(ImGui::GetID(AIM_TEXT("engaged")), false);
        float reveal = storage->GetFloat(revealKey, 0);
        reveal +=
            ((hovered || engaged ? 1.f : 0.f) - reveal) * (motion ? 1 - std::exp(-ImGui::GetIO().DeltaTime * 18) : 1);
        storage->SetFloat(revealKey, reveal);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0, 0, 0, 0});
        ImGui::SetNextItemWidth(sliderWidth);
        change = ImGui::SliderFloat(AIM_TEXT("##slider"), &q.value, q.lo, q.hi, "", ImGuiSliderFlags_AlwaysClamp);
        bool sliderActive = ImGui::IsItemActive() || ImGui::IsItemFocused();
        ImGui::PopStyleColor(5);
        ImGui::SameLine(0, 9 * scale);
        ImGui::SetNextItemWidth(77 * scale);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, mix(ImVec4(0, 0, 0, 0), p.bg, reveal));
        change |= ImGui::InputFloat(AIM_TEXT("##number"), &q.value, 0, 0, q.kind == Integer ? AIM_TEXT("%.0f") : AIM_TEXT("%.2f"));
        storage->SetBool(ImGui::GetID(AIM_TEXT("engaged")), sliderActive || ImGui::IsItemActive());
        ImGui::PopStyleColor();
        if (change) {
            if (!std::isfinite(q.value)) q.value = q.lo;
            q.value = std::clamp(q.value, q.lo, q.hi);
            if (q.kind == Integer) q.value = roundf(q.value);
        }
        float ratio = std::clamp((q.value - q.lo) / std::max(.0001f, q.hi - q.lo), 0.f, 1.f);
        float cy = pos.y + ImGui::GetFrameHeight() * .5f;
        float begin = pos.x + 6 * scale, end = pos.x + sliderWidth - 6 * scale;
        if (reveal > .01f)
        {
            d->AddLine({begin, cy}, {end, cy}, col(p.track, reveal), 2 * scale);
            d->AddLine({begin, cy}, {begin + (end - begin) * ratio, cy}, col(accent, reveal), 2 * scale);
            d->AddCircleFilled({begin + (end - begin) * ratio, cy}, 4 * scale, col(p.text, reveal));
        }
    }
    if (ImGui::IsItemHovered() && !q.hint.empty())
        ImGui::SetTooltip("%s", q.hint.c_str());
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy({width, h});
    ImGui::GetWindowDrawList()->AddLine({a.x, a.y + h}, {a.x + width, a.y + h}, col(p.border, .45f));
    ImGui::PopID();
    return change;
}
void rows(std::initializer_list<const char *> keys)
{
    for (auto key : keys)
        row(key);
}
bool button(const char *label, float width = 0, bool primary = false)
{
    if (primary)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, accent);
        ImGui::PushStyleColor(ImGuiCol_Text, {.07f, .11f, .18f, 1});
    }
    bool b = ImGui::Button(label, {width * scale, 32 * scale});
    if (primary)
        ImGui::PopStyleColor(2);
    return b;
}
void tabs(std::initializer_list<const char *> names)
{
    int i = 0;float selectedX=0,selectedWidth=0,lineY=0;
    for (auto name : names)
    {
        if (i)
            ImGui::SameLine(0, 8 * scale);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, i == subtab ? p.select : ImVec4(0, 0, 0, 0));
        if (button(name))
        {
            if(subtab!=i)requestedSubtab=i;
        }
        if(i==subtab){selectedX=(ImGui::GetItemRectMin().x-ImGui::GetWindowPos().x)/scale;
            selectedWidth=ImGui::GetItemRectSize().x/scale;lineY=ImGui::GetItemRectMax().y+3*scale;}
        ImGui::PopStyleColor();
        ImGui::PopID();
        i++;
    }
    const float blend=motion?1-std::exp(-ImGui::GetIO().DeltaTime*18):1;
    if(!tabLineReady[page]){tabLineX[page]=selectedX;tabLineWidth[page]=selectedWidth;tabLineReady[page]=true;}
    tabLineX[page]+=(selectedX-tabLineX[page])*blend;
    tabLineWidth[page]+=(selectedWidth-tabLineWidth[page])*blend;
    const float lineX=ImGui::GetWindowPos().x+tabLineX[page]*scale;
    ImGui::GetWindowDrawList()->AddRectFilled({lineX,lineY},{lineX+tabLineWidth[page]*scale,lineY+2*scale},col(accent),scale);
    ImGui::Dummy({0, 10 * scale});
}
int changes()
{
    int n = 0;
    for (size_t i = 0; i < fields.size(); i++)
        if (!sameField(fields[i],applied[i]))
            n++;
    return n;
}
void quick(const char *key, int type, float width)
{
    auto &q = f(key);
    ImGui::PushID(key);
    auto a = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(AIM_TEXT("card"), {width, 66 * scale}))
        q.value = q.value ? 0.f : 1.f;
    auto d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, {a.x + width, a.y + 66 * scale}, col(ImGui::IsItemHovered() ? p.hover : p.card), 12 * scale);
    d->AddRect(a, {a.x + width, a.y + 66 * scale}, col(p.border, .65f), 12 * scale);
    d->AddRectFilled(at(a, 13, 16), at(a, 47, 50), col(q.value ? p.select : p.bg), 10 * scale);
    icon(at(a, 30, 33), type, q.value ? accent : p.muted, 8);
    text(at(a, 58, 15), q.label.c_str(), 13.5f, p.text);
    text(at(a, 58, 38), q.value ? "已启用" : "未启用", 10.5f, p.muted);
    // One hit target for the whole card prevents overlapping buttons swallowing clicks.
    auto storage = ImGui::GetStateStorage();
    ImGuiID animationId = ImGui::GetID(AIM_TEXT("card-animation"));
    float t = storage->GetFloat(animationId, q.value ? 1.f : 0.f);
    t += ((q.value ? 1.f : 0.f) - t) * (motion ? 1 - std::exp(-ImGui::GetIO().DeltaTime * 18) : 1);
    storage->SetFloat(animationId, t);
    ImVec2 togglePos{a.x + width - 49 * scale, a.y + 23 * scale};
    d->AddRectFilled(togglePos, at(togglePos, 36, 21), col(mix(p.track, accent, t)), 11 * scale);
    d->AddCircleFilled(at(togglePos, 10.5f + 15 * t, 10.5f), 7.1f * scale, col({.97f, .98f, 1, 1}));
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy({width, 66 * scale});
    ImGui::PopID();
}
void fpsChart()
{
    beginCard(AIM_TEXT("fps-history"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(AIM_TEXT("实时帧率"));
    ImGui::SameLine();
    ImGui::TextDisabled(AIM_TEXT("界面 FPS"));
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 243 * scale);
    for (int seconds : {30, 60, 120})
    {
        ImGui::PushID(seconds);
        ImGui::PushStyleColor(ImGuiCol_Button, fpsRange == seconds ? p.select : ImVec4(0, 0, 0, 0));
        char label[12];
        snprintf(label, sizeof(label), "%ds", seconds);
        if (button(label, 49))
            fpsRange = seconds;
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::SameLine(0, 5 * scale);
    }
    if (button(freezeFps ? "继续" : "冻结", 68))
    {
        freezeFps = !freezeFps;
        if (freezeFps)
        {
            frozenFps = fpsHistory.samples;
            frozenTime = fpsHistory.clock;
        }
    }
    const auto &history = freezeFps ? frozenFps : fpsHistory.samples;
    const double now = freezeFps ? frozenTime : fpsHistory.clock;
    std::vector<FpsSample> visible;
    for (const auto &sample : history)
        if (sample.time >= now - fpsRange && sample.time <= now)
            visible.push_back(sample);
    float minimum = FLT_MAX, maximum = 0, sum = 0;
    for (const auto &sample : visible)
    {
        minimum = std::min(minimum, sample.value);
        maximum = std::max(maximum, sample.value);
        sum += sample.value;
    }
    auto a = ImGui::GetCursorScreenPos();
    width = ImGui::GetContentRegionAvail().x;
    const float height = 185 * scale;
    auto d = ImGui::GetWindowDrawList();
    ImVec2 top = at(a, 40, 10), bottom{a.x + width - 8 * scale, a.y + height - 26 * scale};
    float ceiling = std::max(10.f, std::ceil(maximum * 1.12f / 10) * 10);
    for (int i = 0; i <= 4; ++i)
    {
        float y = bottom.y - (bottom.y - top.y) * i / 4;
        d->AddLine({top.x, y}, {bottom.x, y}, col(p.border, .55f));
        char label[20];
        snprintf(label, sizeof(label), AIM_TEXT("%.0f"), ceiling * i / 4);
        text({a.x, y - 7 * scale}, label, 9, p.muted);
    }
    text({top.x, bottom.y + 8 * scale}, ("-" + std::to_string(fpsRange) + "s").c_str(), 9, p.muted);
    text({(top.x + bottom.x) * .5f - 12 * scale, bottom.y + 8 * scale},
         ("-" + std::to_string(fpsRange / 2) + "s").c_str(), 9, p.muted);
    text({bottom.x - 26 * scale, bottom.y + 8 * scale}, freezeFps ? "冻结" : "现在", 9, p.muted);
    std::vector<ImVec2> points;
    for (const auto &sample : visible)
        points.push_back({top.x + static_cast<float>((sample.time - now + fpsRange) / fpsRange) * (bottom.x - top.x),
                          bottom.y - sample.value / ceiling * (bottom.y - top.y)});
    for (size_t i = 1; i < points.size(); ++i)
        if (!visible[i].gap)
            d->AddLine(points[i - 1], points[i], col(accent), 2 * scale);
    if (!points.empty())
        d->AddCircleFilled(points.back(), 3 * scale, col(accent));
    else
        text({top.x + 30 * scale, top.y + 40 * scale}, AIM_TEXT("等待首个帧率样本…"), 12, p.muted);
    ImGui::InvisibleButton(AIM_TEXT("fps-plot"), {width, height});
    if (ImGui::IsItemHovered() && !points.empty())
    {
        size_t nearest = 0;
        for (size_t i = 1; i < points.size(); ++i)
            if (std::abs(points[i].x - ImGui::GetIO().MousePos.x) <
                std::abs(points[nearest].x - ImGui::GetIO().MousePos.x))
                nearest = i;
        const auto point = points[nearest];
        d->AddLine({point.x, top.y}, {point.x, bottom.y}, col(p.muted, .6f));
        d->AddCircleFilled(point, 4 * scale, col(p.text));
        ImGui::SetTooltip(AIM_TEXT("%.2f 秒前\n%.1f FPS"), now - visible[nearest].time, visible[nearest].value);
    }
    if (visible.empty())
        ImGui::TextDisabled(AIM_TEXT("每 250 ms 采样 · 切换页面后继续记录"));
    else
        ImGui::TextDisabled(AIM_TEXT("最低 %.1f    平均 %.1f    最高 %.1f    当前 %.1f"), minimum,
                            sum / static_cast<float>(visible.size()), maximum, visible.back().value);
    endCard();
}
void overview()
{
    tabs({AIM_TEXT("快捷功能"), AIM_TEXT("常用调节"), AIM_TEXT("工作区预览")});
    if (subtab == 1)
    {
        section(AIM_TEXT("常用调节"));
        beginCard(AIM_TEXT("overview_parameters"));
        rows({AIM_TEXT("conf"), AIM_TEXT("aim_r"), AIM_TEXT("proportion"), "kd", AIM_TEXT("predict_x"), AIM_TEXT("rate")});
        endCard();
        return;
    }
    if (subtab == 2)
    {
        beginCard(AIM_TEXT("workspace"));
        if (ImGui::BeginTable(AIM_TEXT("workspace-columns"), 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::TextDisabled(AIM_TEXT("采集方式"));
            ImGui::TextUnformatted(options(f(AIM_TEXT("cap_backend")).options)[std::clamp((int)f(AIM_TEXT("cap_backend")).value,0,5)].c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled(AIM_TEXT("执行设备"));
            ImGui::TextUnformatted(options(f(AIM_TEXT("device_type")).options)[std::clamp((int)f(AIM_TEXT("device_type")).value,0,2)].c_str());
            ImGui::EndTable();
        }
        ImGui::Dummy({0, 4 * scale});
        ImGui::TextDisabled(AIM_TEXT("当前模型"));
        {
            auto model = std::string(f(AIM_TEXT("model_path")).text.data());
            auto split = model.find_last_of("/\\");
            if (split != std::string::npos)
                model = model.substr(split + 1);
            if (model.empty())
                model = AIM_TEXT("未选择模型");
            float modelWidth = ImGui::GetContentRegionAvail().x - 14 * scale;
            std::string shortName = model;
            while (shortName.size() > 3 && ImGui::CalcTextSize(shortName.c_str()).x > modelWidth)
            {
                // Remove a whole UTF-8 code point when shortening a model name.
                size_t cut = shortName.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(shortName[cut]) & 0xc0) == 0x80)
                    --cut;
                shortName.resize(cut);
            }
            ImGui::TextUnformatted(shortName.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", f(AIM_TEXT("model_path")).text.data());
        }
        endCard();
        fpsChart();
        return;
    }
    if (ImGui::BeginTable(AIM_TEXT("quick-grid"), 2, ImGuiTableFlags_SizingStretchSame))
    {
        const char *keys[] = {AIM_TEXT("continuous_lock"), AIM_TEXT("preview_win"), AIM_TEXT("auto_fire"), AIM_TEXT("findcolor"), AIM_TEXT("hotkey1_enabled"), "cs"};
        const int icons[] = {1, 4, 1, 3, 2, 4};
        for (int i = 0; i < 6; i++)
        {
            ImGui::TableNextColumn();
            quick(keys[i], icons[i], ImGui::GetContentRegionAvail().x);
        }
        ImGui::EndTable();
    }
}
float aimFraction(float percent)
{
    return (std::clamp(percent, -100.f, 100.f) + 100.f) / 200.f;
}
void calibration()
{
    auto a = ImGui::GetCursorScreenPos();
    auto d = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    float h = std::clamp(ImGui::GetIO().DisplaySize.y / scale - 390.f, 220.f, 260.f) * scale;
    d->AddRectFilled(a, {a.x + w, a.y + h}, col(p.bg), 14 * scale);
    const float top = a.y + 25 * scale, bodyHeight = h - 50 * scale;
    const float bodyWidth = bodyHeight * 864.f / 1520.f;
    const float center = a.x + w * .5f;
    ImVec2 bodyMin{center - bodyWidth * .5f, top}, bodyMax{center + bodyWidth * .5f, top + bodyHeight};
    for (int i = 0; i < 5; i++)
    {
        float yy = top + bodyHeight * i / 4;
        d->AddLine({a.x + 60 * scale, yy}, {a.x + w - 60 * scale, yy}, col(p.border, .35f));
        char label[12];
        snprintf(label, sizeof(label), "%+d", i * 50 - 100);
        text({a.x + 16 * scale, yy - 7 * scale}, label, 10, p.muted);
    }
    if (humanTexture)
        d->AddImage(ImTextureRef(humanTexture), bodyMin, bodyMax, {0, 0}, {1, 1},
                    col(mix({1, 1, 1, .9f}, {.43f, .52f, .62f, .92f}, themeBlend)));
    ImGui::InvisibleButton(AIM_TEXT("calibration-image"), {w, h});
    if (ImGui::IsItemActive())
        f(AIM_TEXT("aim_pct")).value = std::clamp((ImGui::GetIO().MousePos.y - top) / bodyHeight * 200.f - 100.f, -100.f, 100.f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(AIM_TEXT("拖动人物区域调整瞄点；负值向上，正值向下"));
    float yy = top + bodyHeight * aimFraction(f(AIM_TEXT("aim_pct")).value);
    d->AddLine({center - bodyWidth * .5f - 18 * scale, yy}, {center + bodyWidth * .5f + 18 * scale, yy},
               col(accent, .5f), scale);
    d->AddCircleFilled({center, yy}, 14 * scale, col(accent, .12f));
    d->AddCircle({center, yy}, 8 * scale, col(accent), 32, 1.5f * scale);
    d->AddCircleFilled({center, yy}, 2.5f * scale, col(p.text));
    d->AddLine({center - 18 * scale, yy}, {center - 11 * scale, yy}, col(accent), 1.5f * scale);
    d->AddLine({center + 11 * scale, yy}, {center + 18 * scale, yy}, col(accent), 1.5f * scale);
    d->AddLine({center, yy - 18 * scale}, {center, yy - 11 * scale}, col(accent), 1.5f * scale);
    d->AddLine({center, yy + 11 * scale}, {center, yy + 18 * scale}, col(accent), 1.5f * scale);
    text({a.x + w - 105 * scale, a.y + 13 * scale}, AIM_TEXT("配置示意"), 10, p.muted);
    const char *region = f(AIM_TEXT("aim_pct")).value <= -80  ? "头部"
                         : f(AIM_TEXT("aim_pct")).value >= 80 ? "脚部"
                         : f(AIM_TEXT("aim_pct")).value < -12 ? "上半身"
                         : f(AIM_TEXT("aim_pct")).value > 12  ? "下半身"
                                                    : AIM_TEXT("人物中心");
    char readout[96];
    snprintf(readout, sizeof(readout), AIM_TEXT("%s  ·  %+.0f%%"), region, f(AIM_TEXT("aim_pct")).value);
    text({a.x + w - 150 * scale, a.y + h - 30 * scale}, readout, 10, p.muted);
}
void aimPage()
{
    tabs({AIM_TEXT("瞄点校准"), AIM_TEXT("激活热键"), AIM_TEXT("开火保护"), AIM_TEXT("目标筛选")});
    if (subtab == 0)
    {
        beginCard(AIM_TEXT("aimpoint"));
        calibration();
        rows({AIM_TEXT("aim_pct"), AIM_TEXT("y_off")});
        if (button("头部", 68))
            f(AIM_TEXT("aim_pct")).value = -85;
        ImGui::SameLine();
        if (button("胸口", 68))
            f(AIM_TEXT("aim_pct")).value = -30;
        ImGui::SameLine();
        if (button("中心", 68))
            f(AIM_TEXT("aim_pct")).value = 0;
        ImGui::SameLine();
        if (button("复位", 68))
        {
            f(AIM_TEXT("aim_pct")).value = 0;
            f(AIM_TEXT("y_off")).value = 0;
        }
        endCard();
    }
    else if (subtab == 3)
    {
        section(AIM_TEXT("目标筛选"));
        beginCard(AIM_TEXT("target"));
        rows({AIM_TEXT("conf"), AIM_TEXT("categories"), AIM_TEXT("aim_r")});
        if (button(AIM_TEXT("刷新模型类别"))) refreshClasses();
        ImGui::SameLine();
        if (button(AIM_TEXT("查看类别"))) ImGui::OpenPopup(AIM_TEXT("classes"));
        if (ImGui::BeginPopup(AIM_TEXT("classes")))
        {
            if (modelClasses.empty()) ImGui::TextUnformatted(AIM_TEXT("当前模型无类别元数据，请加载模型后刷新"));
            for (const auto& [id,name] : modelClasses) ImGui::Text(AIM_TEXT("%s  %s"), id.c_str(), name.c_str());
            ImGui::TextDisabled(AIM_TEXT("类别 ID 从左到右优先；留空表示不选择目标。"));
            ImGui::EndPopup();
        }
        endCard();
    }
    else if (subtab == 1)
    {
        section(AIM_TEXT("激活方式"));
        beginCard(AIM_TEXT("keys"));
        rows({AIM_TEXT("continuous_lock"), AIM_TEXT("hotkey1_enabled"), AIM_TEXT("hotkey1"), AIM_TEXT("hotkey2_enabled"), AIM_TEXT("hotkey2")});
        endCard();
        section(AIM_TEXT("自动开火"));beginCard(AIM_TEXT("fire-hotkey"));
        row(AIM_TEXT("auto_fire"));
        ImGui::BeginDisabled(f(AIM_TEXT("auto_fire")).value==0);row(AIM_TEXT("auto_fire_button"));ImGui::EndDisabled();
        ImGui::TextDisabled(AIM_TEXT("目标进入开火范围后，自动点击所选鼠标键。"));
        endCard();
    }
    else
    {
        section(AIM_TEXT("识别与开火"));
        beginCard(AIM_TEXT("fire"));
        row(AIM_TEXT("fire_mode"));
        row(f(AIM_TEXT("fire_mode")).value ? AIM_TEXT("fire_box_ratio") : AIM_TEXT("fire_r"));
        endCard();
        section(AIM_TEXT("急停保护"));
        beginCard(AIM_TEXT("safety"));
        rows({"cs", AIM_TEXT("cs_backend")});
        endCard();
    }
}
void motionPage()
{
    tabs({"响应", "预测", "跟踪", "曲线", "高级"});
    if (subtab == 0)
    {
        section(AIM_TEXT("响应参数"), AIM_TEXT("PID RESPONSE"));
        beginCard("pid");
        rows({AIM_TEXT("proportion"), "kd", AIM_TEXT("rate")});
        endCard();
    }
    else if (subtab == 1)
    {
        section(AIM_TEXT("预测与平滑"));
        beginCard(AIM_TEXT("predict"));
        rows({AIM_TEXT("predict_x"), AIM_TEXT("predict_y"), AIM_TEXT("vel_alpha")});
        endCard();
    }
    else if (subtab == 2)
    {
        section(AIM_TEXT("跟踪连续性"));
        beginCard(AIM_TEXT("tracking"));
        rows({AIM_TEXT("max_drop_frames"), AIM_TEXT("max_compensate_frames"), AIM_TEXT("min_lock_frames"), AIM_TEXT("same_dist")});
        endCard();
    }
    else if (subtab == 4)
    {
        section(AIM_TEXT("高级输出参数"));
        beginCard(AIM_TEXT("advanced"));
        rows({AIM_TEXT("smooth"), AIM_TEXT("out_max"), "dz"});
        endCard();
    }
    else
    {
        beginCard(AIM_TEXT("curve"));
        int mode = f(AIM_TEXT("nn_enabled")).value ? 2 : f(AIM_TEXT("bez_enabled")).value ? 1 : 0;
        const char *modes[] = {"不启用", "贝塞尔", AIM_TEXT("神经网络")};
        for (int i = 0; i < 3; i++)
        {
            if (i)
                ImGui::SameLine();
            if (ImGui::RadioButton(modes[i], mode == i))
            {
                f(AIM_TEXT("bez_enabled")).value = i == 1;
                f(AIM_TEXT("nn_enabled")).value = i == 2;
                mode = i;
            }
        }
        auto a = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        auto d = ImGui::GetWindowDrawList();
        d->AddRectFilled(a, {a.x + w, a.y + 70 * scale}, col(p.bg), 12 * scale);
        ImVec2 from = at(a, 20, 52), to = {a.x + w - 20 * scale, a.y + 18 * scale};
        if (mode)
            d->AddBezierCubic(from, {a.x + w * .4f, a.y + 64 * scale}, {a.x + w * .6f, a.y + 2 * scale}, to,
                              col(accent), 2 * scale);
        else
            d->AddLine(from, to, col(accent), 2 * scale);
        d->AddCircleFilled(from, 3 * scale, col(p.text));
        d->AddCircleFilled(to, 3 * scale, col(p.text));
        ImGui::Dummy({w, 78 * scale});
        if (mode == 1)
        {

            if (button(AIM_TEXT("路径参数"), 100))
                curvePart = 0;
            ImGui::SameLine();
            if (button(AIM_TEXT("时间参数"), 100))
                curvePart = 1;
            if (curvePart == 0)
                rows({AIM_TEXT("bez_dist"), AIM_TEXT("bez_dev"), AIM_TEXT("bez_direct")});
            else
                rows({AIM_TEXT("bez_min_d"), AIM_TEXT("bez_max_d"), AIM_TEXT("bez_dur"), AIM_TEXT("bez_step")});
        }
        else if (mode == 2)
            rows({AIM_TEXT("nn_dist"), AIM_TEXT("nn_step")});
        else
            ImGui::TextDisabled(AIM_TEXT("选择曲线类型后展开对应参数"));
        endCard();
    }
}
struct HueState
{
    float start = 177, end = 3;
    bool manual = false;
};
HueState inferHue()
{
    float a = f(AIM_TEXT("fc_h1lo")).value, b = f(AIM_TEXT("fc_h1hi")).value, c = f(AIM_TEXT("fc_h2lo")).value, d = f(AIM_TEXT("fc_h2hi")).value;
    if ((a == c && b == d) || a > b)
        return {a, b, false};
    if (a == 0 && d == 180 && c >= b)
        return {c, b, false};
    if (b == 180 && c == 0 && a >= d)
        return {a, d, false};
    return {a, b, true};
}
void writeHue(float start, float end)
{
    start = std::clamp(roundf(start), 0.f, 180.f);
    end = std::clamp(roundf(end), 0.f, 180.f);
    f(AIM_TEXT("fc_h1lo")).value = start;
    f(AIM_TEXT("fc_h1hi")).value = start > end ? 180 : end;
    f(AIM_TEXT("fc_h2lo")).value = start > end ? 0 : start;
    f(AIM_TEXT("fc_h2hi")).value = end;
}
ImVec2 huePoint(ImVec2 center, float radius, float hue)
{
    float angle = hue / 180.f * 6.2831853f - 1.5707963f;
    return {center.x + cosf(angle) * radius, center.y + sinf(angle) * radius};
}
void hueArc(ImVec2 center, float radius, float start, float end, ImU32 color, float thickness)
{
    float span = end - start;
    if (span < 0)
        span += 180;
    if (span < .01f)
        return;
    auto d = ImGui::GetWindowDrawList();
    int segments = std::max(2, (int)(span * 1.5f));
    for (int i = 0; i <= segments; i++)
        d->PathLineTo(huePoint(center, radius, start + span * i / segments));
    d->PathStroke(color, 0, thickness);
}
void hueRing()
{
    auto a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const float size = 240 * scale;
    ImVec2 center{a.x + w * .5f, a.y + size * .5f};
    float radius = 91 * scale;
    auto d = ImGui::GetWindowDrawList();
    HueState state = inferHue();
    static int endpoint = 0; // Explicit selection also makes coincident endpoints reachable.
    d->AddCircle(center, 113 * scale, col(p.border), 96, scale);
    for (int i = 0; i < 60; i++)
        d->AddLine(huePoint(center, 107 * scale, i * 3.f), huePoint(center, (i % 5 ? 109 : 112) * scale, i * 3.f),
                   col(p.muted, .5f), scale);
    for (int i = 0; i < 360; i++)
    {
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(i / 360.f, .78f, .96f, r, g, b);
        hueArc(center, radius, i * .5f, (i + 1) * .5f + .15f, col({r, g, b, 1}), 14 * scale);
    }
    if (state.manual)
    {
        hueArc(center, 102 * scale, f(AIM_TEXT("fc_h1lo")).value, f(AIM_TEXT("fc_h1hi")).value, col(accent), 3 * scale);
        hueArc(center, 117 * scale, f(AIM_TEXT("fc_h2lo")).value, f(AIM_TEXT("fc_h2hi")).value, col(accent, .6f), 3 * scale);
    }
    else
        hueArc(center, 102 * scale, state.start, state.end, col(p.text), 2 * scale);
    ImGui::InvisibleButton(AIM_TEXT("hue-ring"), {w, size});
    auto mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated())
    {
        ImVec2 p1 = huePoint(center, radius, state.start), p2 = huePoint(center, radius, state.end);
        float d1 = (mouse.x - p1.x) * (mouse.x - p1.x) + (mouse.y - p1.y) * (mouse.y - p1.y);
        float d2 = (mouse.x - p2.x) * (mouse.x - p2.x) + (mouse.y - p2.y) * (mouse.y - p2.y);
        if (std::abs(d1 - d2) > 16 * scale * scale)
            endpoint = d1 < d2 ? 0 : 1;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0))
    {
        float angle = atan2f(mouse.y - center.y, mouse.x - center.x) + 1.5707963f;
        if (angle < 0)
            angle += 6.2831853f;
        float hue = std::clamp(roundf(angle / 6.2831853f * 180), 0.f, 180.f);
        if (endpoint == 0)
            state.start = hue;
        else
            state.end = hue;
        writeHue(state.start, state.end);
        state.manual = false;
    }
    if (ImGui::IsItemFocused())
    {
        float step = ImGui::GetIO().KeyShift ? 5.f : 1.f;
        float &value = endpoint == 0 ? state.start : state.end;
        bool changed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            value -= step;
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            value += step;
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home))
        {
            value = 0;
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End))
        {
            value = 180;
            changed = true;
        }
        if (changed)
            writeHue(state.start, state.end);
    }
    state = inferHue();
    for (int i = 0; i < 2; i++)
    {
        ImVec2 pt = huePoint(center, radius, i ? state.end : state.start);
        d->AddCircleFilled(pt, 9 * scale, col(p.bg));
        d->AddCircle(pt, 9 * scale, col(i == endpoint ? p.text : accent), 32, 2 * scale);
        d->AddCircleFilled(pt, 3 * scale, col(i == 0 ? accent : p.text));
    }
    char label[64];
    snprintf(label, sizeof(label), AIM_TEXT("H %.0f  →  %.0f"), state.start, state.end);
    ImVec2 extent = regular->CalcTextSizeA(18 * scale, FLT_MAX, 0, label);
    text({center.x - extent.x * .5f, center.y - 18 * scale}, label, 16.5f, p.text);
    const char *note = state.manual ? AIM_TEXT("手动双区") : state.start > state.end ? AIM_TEXT("跨界自动拆分") : AIM_TEXT("连续色相范围");
    text({center.x - 42 * scale, center.y + 13 * scale}, note, 10.5f, p.muted);
    if (button(endpoint == 0 ? AIM_TEXT("● 起点") : "起点", 94))
        endpoint = 0;
    ImGui::SameLine();
    if (button(endpoint == 1 ? AIM_TEXT("● 终点") : "终点", 94))
        endpoint = 1;
    ImGui::SameLine();
    ImGui::TextDisabled(AIM_TEXT("拖动端点 · Shift 微调 5"));
}
bool rangeInput(const char *label, float &lo, float &hi)
{
    ImGui::PushID(label);
    float width = ImGui::GetContentRegionAvail().x;
    float start = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(start + width - 236 * scale);
    ImGui::SetNextItemWidth(236 * scale);
    int values[] = {static_cast<int>(lo), static_cast<int>(hi)};
    bool changed = ImGui::InputInt2(AIM_TEXT("##range"), values);
    if (changed)
    {
        lo = static_cast<float>(std::clamp(values[0], 0, 255));
        hi = static_cast<float>(std::clamp(values[1], 0, 255));
        if (lo > hi)
            std::swap(lo, hi);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(AIM_TEXT("左侧下限，右侧上限 · 0–255"));
    ImGui::PopID();
    return changed;
}
void colorPage()
{
    tabs({AIM_TEXT("色相圆环"), AIM_TEXT("区域辅助"), AIM_TEXT("范围 1"), AIM_TEXT("范围 2")});
    if (subtab == 0)
    {
        beginCard("hue");
        hueRing();
        // Read from draft every frame so import/reset cannot leave stale quick values.
        float slo = f(AIM_TEXT("fc_s1lo")).value, shi = f(AIM_TEXT("fc_s1hi")).value, vlo = f(AIM_TEXT("fc_v1lo")).value, vhi = f(AIM_TEXT("fc_v1hi")).value;
        if (rangeInput(AIM_TEXT("饱和度 S"), slo, shi))
        {
            f(AIM_TEXT("fc_s1lo")).value = slo;
            f(AIM_TEXT("fc_s1hi")).value = shi;
        }
        if (rangeInput(AIM_TEXT("明度 V"), vlo, vhi))
        {
            f(AIM_TEXT("fc_v1lo")).value = vlo;
            f(AIM_TEXT("fc_v1hi")).value = vhi;
        }
        if (button(AIM_TEXT("同步 S / V 到双区")))
        {
            for (auto channel : {"s", "v"})
                for (auto bound : {"lo", "hi"})
                    f((std::string("fc_") + channel + "2" + bound).c_str()).value =
                        f((std::string("fc_") + channel + "1" + bound).c_str()).value;
            notify(AIM_TEXT("已同步 S / V，色相范围保持不变"));
        }
        ImGui::TextDisabled(inferHue().manual ? AIM_TEXT("保留独立双区；拖动圆环将改为连续选区。")
                                              : AIM_TEXT("拖动圆环只调整 H，两组 S / V 保持各自设置。"));
        endCard();
    }
    else if (subtab == 1)
    {
        section(AIM_TEXT("找色辅助"));
        beginCard(AIM_TEXT("colorbase"));
        rows({AIM_TEXT("findcolor"), AIM_TEXT("fc_radius"), AIM_TEXT("fc_aim_scale"), AIM_TEXT("fc_min_area")});
        endCard();
    }
    else
    {
        section(subtab == 2 ? AIM_TEXT("HSV 范围 1") : AIM_TEXT("HSV 范围 2"));
        beginCard("hsv");
        std::string n = std::to_string(subtab - 1);
        for (auto c : {"h", "s", "v"})
        {
            row(("fc_" + std::string(c) + n + "lo").c_str());
            row(("fc_" + std::string(c) + n + "hi").c_str());
        }
        endCard();
    }
}
std::string fileDialog(HWND window, bool save, const wchar_t *filter, const wchar_t *ext)
{
    wchar_t path[1024] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = window;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = ext;
    std::wstring initialDirectory = NativeBackend::Directory() + (wcscmp(ext,AIM_TEXT(L"onnx"))==0 ? AIM_TEXT(L"models") : L"配置");
    ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)))
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}
std::wstring wide(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    out.pop_back();
    return out;
}
void devicePage(HWND window)
{
    tabs({AIM_TEXT("执行设备"), AIM_TEXT("图像采集"), AIM_TEXT("模型推理"), AIM_TEXT("移动测试"), AIM_TEXT("画面预览")});
    if(subtab==0) {
        section(AIM_TEXT("执行设备"));beginCard(AIM_TEXT("device"));row(AIM_TEXT("device_type"));
        int type=(int)f(AIM_TEXT("device_type")).value;
        if(type==0)rows({AIM_TEXT("km_ip"),AIM_TEXT("km_port"),AIM_TEXT("km_mac")});
        else if(type==1){row(AIM_TEXT("makcu_port"));ImGui::TextDisabled(AIM_TEXT("Compatible SDK / negotiated 4M"));
            ImGui::TextWrapped(AIM_TEXT("makcu-cpp@9874efa96f58e7a768e0110d765f0fa072e7e25e · ASCII"));
            ImGui::TextDisabled(AIM_TEXT("connect(port,true): 115200 -> negotiated 4000000"));}
        else ImGui::TextDisabled(backendState.moveMethod==2&&backendState.connected?AIM_TEXT("独家驱动已就绪"):AIM_TEXT("独家驱动未就绪"));
        ImGui::Spacing();
        if(button(connected?AIM_TEXT("重新连接"):AIM_TEXT("连接设备"),124,true))requestConnect();
        ImGui::SameLine();
        if(button(AIM_TEXT("链路状态")))ImGui::OpenPopup(AIM_TEXT("diag"));
        if(ImGui::BeginPopup(AIM_TEXT("diag"))) {
            const auto& c=backendState.connection;
            ImGui::Text(AIM_TEXT("连接请求 #%u · 状态 %d"),c.requestId,c.state);
            ImGui::Text(AIM_TEXT("实际执行设备：%d"),backendState.moveMethod);
            ImGui::Text(AIM_TEXT("采集后端：%d"),backendState.captureBackend);
            ImGui::TextUnformatted(running?AIM_TEXT("处理链路运行中"):AIM_TEXT("处理链路暂停"));
            if(c.error[0])ImGui::TextWrapped("%s",c.error);
            ImGui::EndPopup();
        }
        endCard();
    }else if(subtab==3){
        section(AIM_TEXT("移动测试"));beginCard(AIM_TEXT("move"));
        static int dx=100,dy=0;
        const char* labels[]={"向左","向上","向下","向右"};
        const int xs[]={-100,0,0,100},ys[]={0,-100,100,0};
        if(!canMove())ImGui::TextDisabled(AIM_TEXT("连接设备并暂停处理后可测试"));
        ImGui::BeginDisabled(!canMove());
        for(int i=0;i<4;++i){if(i)ImGui::SameLine();if(button(labels[i],70))requestMove(xs[i],ys[i]);}
        ImGui::SetNextItemWidth(130*scale);ImGui::InputInt("X",&dx);
        ImGui::SameLine();ImGui::SetNextItemWidth(130*scale);ImGui::InputInt("Y",&dy);
        if(button(AIM_TEXT("执行移动"))){
            if(dx<-32768||dx>32767||dy<-32768||dy>32767||(!dx&&!dy))notify(AIM_TEXT("位移须在 -32768 到 32767 之间，且不能同时为零"));
            else requestMove(dx,dy);
        }
        ImGui::EndDisabled();endCard();
    }else if(subtab==1){
        section(AIM_TEXT("图像采集"));beginCard(AIM_TEXT("capture"));row(AIM_TEXT("cap_backend"));int type=(int)f(AIM_TEXT("cap_backend")).value;
        if(type==3)row(AIM_TEXT("capture_udp_port"));
        if(type==5)row(AIM_TEXT("capture_stream_port"));
        if(type==4){
            if(!captureRequested&&!actionsBusy()&&backendLoaded)refreshCapture();
            ImGui::BeginDisabled(captureBusy());
            if(metadataCombo(AIM_TEXT("capture_device"),captureDevices))selectCaptureDevice();
            metadataCombo(AIM_TEXT("capture_mode"),captureModes);
            if(button(AIM_TEXT("刷新设备")))refreshCapture();ImGui::SameLine();
            if(button(AIM_TEXT("刷新格式")))refreshCaptureModes();
            ImGui::EndDisabled();
            ImGui::TextWrapped("%s",captureStatus.c_str());
            if(running)ImGui::TextDisabled(AIM_TEXT("切换后点击应用；格式枚举需先暂停处理"));
        }
        rows({AIM_TEXT("cap_r"),AIM_TEXT("max_capture_fps"),AIM_TEXT("preview_win")});endCard();
    }else if(subtab==4){
        section(AIM_TEXT("画面预览"));beginCard(AIM_TEXT("capture-preview"));row(AIM_TEXT("preview_win"));
        ImGui::TextWrapped(AIM_TEXT("应用并启动后，采集画面显示在原有实时预览窗口中。"));
        ImGui::Text(AIM_TEXT("采集 %.1f FPS · %.2f ms"),running?backendState.telemetry.fps:0,running?backendState.telemetry.captureMs:0);
        if(backendState.captureFailed)ImGui::TextWrapped(AIM_TEXT("采集失败，请检查采集模式和后端日志。"));
        endCard();
    }else{
        section(AIM_TEXT("模型与推理"));beginCard(AIM_TEXT("model"));row(AIM_TEXT("model_path"));
        if(button(AIM_TEXT("选择 ONNX 模型"))){
            auto selected=fileDialog(window,false,AIM_TEXT(L"ONNX model\0*.onnx\0"),AIM_TEXT(L"onnx"));
            if(!selected.empty()){
                if(selected.size()>=512)notify(AIM_TEXT("模型路径过长"));
                else {readField(f(AIM_TEXT("model_path")),selected);notify(AIM_TEXT("已选择模型，应用或启动后生效"));}
            }
        }
        ImGui::SameLine();if(button(AIM_TEXT("刷新列表")))nextBackendPoll=0;
        if(ImGui::BeginCombo(AIM_TEXT("本地模型"),f(AIM_TEXT("model_path")).text.data())){
            for(const auto& item:modelFiles){auto full=utf8(item),label=utf8(std::filesystem::path(item).filename().wstring());
                if(ImGui::Selectable((label+"##"+full).c_str(),full==f(AIM_TEXT("model_path")).text.data())){
                    if(full.size()<512)readField(f(AIM_TEXT("model_path")),full);else notify(AIM_TEXT("模型路径过长"));
                }}
            ImGui::EndCombo();
        }
        ImGui::TextUnformatted(AIM_TEXT("ONNX Runtime / DirectML"));
        if(ImGui::CollapsingHeader(AIM_TEXT("兼容参数"))){
            ImGui::BeginDisabled();rows({AIM_TEXT("inference_engine"),AIM_TEXT("onnx_device")});ImGui::EndDisabled();
        }
        endCard();if(button(AIM_TEXT("加载模型并启动"),180,true))requestStart();
    }
}

std::string snapshot() {
    std::string out="{\n";
    for(size_t i=0;i<fields.size();++i){const auto& q=fields[i];
        out+="  "+NativeJson::quote(q.key)+": "+(q.kind==Text?NativeJson::quote(q.text.data()):parameterValue(q));
        out+=(i+1<fields.size()?",\n":"\n");
    }return out+"}\n";
}
bool importSnapshot(const std::string& input) {
    try {
        auto json=NativeJson::parse(input);if(json.type!=NativeJson::Value::Object)return false;
        auto candidate=fields;
        for(const auto& [key,value]:json.object){
            auto it=std::find_if(candidate.begin(),candidate.end(),[&](const Field& q){return q.key==(key==AIM_TEXT("auto_fire_hotkey")?AIM_TEXT("auto_fire_button"):key);});
            if(it==candidate.end())return false;
            if(it->kind==Text && value.type!=NativeJson::Value::String)return false;
            if(it->kind!=Text && value.type!=NativeJson::Value::Number && value.type!=NativeJson::Value::Bool)return false;
            auto text=value.type==NativeJson::Value::Bool?(value.scalar==AIM_TEXT("true")?"1":"0"):value.text();
            readField(*it,text);
            if(it->kind!=Text){
                if((it->kind==Choice||it->kind==Boolean||it->key==AIM_TEXT("hotkey1")||it->key==AIM_TEXT("hotkey2"))&&(it->value<it->lo||it->value>it->hi))return false;
                if(it->kind!=Number && std::floor(it->value)!=it->value)return false;
            }
        }
        NativeBackend::Parameters proposed;
        for(const auto& q:candidate)proposed[q.key]=parameterValue(q);
        std::string error;if(!NativeBackend::Validate(proposed,error,false))return false;
        fields=std::move(candidate);return true;
    }catch(...){return false;}
}
void resetDraft(){
    const NativeBackend::Parameters values={
        {AIM_TEXT("proportion"),"25"},{"kd","25"},{AIM_TEXT("predict_x"),"1"},{AIM_TEXT("predict_y"),"0"},{AIM_TEXT("rate"),"0.3"},{AIM_TEXT("smooth"),AIM_TEXT("9900")},{"dz","0.5"},{AIM_TEXT("out_max"),"0"},
        {AIM_TEXT("y_off"),"12"},{AIM_TEXT("conf"),AIM_TEXT("0.25")},{AIM_TEXT("auto_fire"),"0"},{AIM_TEXT("auto_fire_button"),"1"},{AIM_TEXT("continuous_lock"),"0"},{AIM_TEXT("fire_mode"),"1"},{AIM_TEXT("fire_r"),"3"},{AIM_TEXT("fire_box_ratio"),"0.8"},
        {AIM_TEXT("aim_pct"),"0"},{AIM_TEXT("cap_r"),"160"},{AIM_TEXT("aim_r"),"160"},{"cs","0"},{AIM_TEXT("cs_backend"),"2"},{AIM_TEXT("hotkey1_enabled"),"0"},{AIM_TEXT("hotkey1"),"1"},
        {AIM_TEXT("hotkey2_enabled"),"1"},{AIM_TEXT("hotkey2"),"2"},{AIM_TEXT("preview_win"),"1"},{AIM_TEXT("max_drop_frames"),"3"},{AIM_TEXT("max_compensate_frames"),"0"},{AIM_TEXT("min_lock_frames"),"1"},
        {AIM_TEXT("vel_alpha"),AIM_TEXT("0.35")},{AIM_TEXT("same_dist"),AIM_TEXT("2500")},{AIM_TEXT("bez_enabled"),"1"},{AIM_TEXT("bez_dist"),"50"},{AIM_TEXT("bez_dev"),AIM_TEXT("0.28")},{AIM_TEXT("nn_enabled"),"0"},{AIM_TEXT("nn_dist"),"15"},{AIM_TEXT("nn_step"),"2"},
        {AIM_TEXT("bez_min_d"),"2"},{AIM_TEXT("bez_max_d"),"5"},{AIM_TEXT("bez_dur"),"10"},{AIM_TEXT("bez_step"),"3"},{AIM_TEXT("bez_direct"),"8"},{AIM_TEXT("findcolor"),"0"},
        {AIM_TEXT("fc_h1lo"),"0"},{AIM_TEXT("fc_h1hi"),"3"},{AIM_TEXT("fc_s1lo"),"100"},{AIM_TEXT("fc_s1hi"),"255"},{AIM_TEXT("fc_v1lo"),"120"},{AIM_TEXT("fc_v1hi"),"255"},
        {AIM_TEXT("fc_h2lo"),"177"},{AIM_TEXT("fc_h2hi"),"180"},{AIM_TEXT("fc_s2lo"),"100"},{AIM_TEXT("fc_s2hi"),"255"},{AIM_TEXT("fc_v2lo"),"100"},{AIM_TEXT("fc_v2hi"),"255"},
        {AIM_TEXT("fc_radius"),"40"},{AIM_TEXT("fc_aim_scale"),"0.7"},{AIM_TEXT("fc_min_area"),"2"},{AIM_TEXT("cap_backend"),"1"},{AIM_TEXT("capture_udp_port"),AIM_TEXT("7788")},{AIM_TEXT("capture_stream_port"),AIM_TEXT("9339")},
        {AIM_TEXT("capture_device"),""},{AIM_TEXT("capture_mode"),""},{AIM_TEXT("inference_engine"),"1"},{AIM_TEXT("onnx_device"),"1"}
    };
    for(const auto& [key,value]:values)readField(f(key.c_str()),value);
}

void configPage(HWND window)
{
    tabs({AIM_TEXT("配置档案"), AIM_TEXT("编辑摘要"), AIM_TEXT("界面外观")});
    if (subtab == 0)
    {
        section(AIM_TEXT("配置档案"));beginCard(AIM_TEXT("profiles"));
        profileIndex=std::clamp(profileIndex,0,std::max(0,(int)profileFiles.size()-1));
        std::string selected=profileFiles.empty()?AIM_TEXT("暂无档案"):utf8(std::filesystem::path(profileFiles[profileIndex]).filename().wstring());
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo(AIM_TEXT("##profiles"),selected.c_str())){
            for(int i=0;i<(int)profileFiles.size();++i){auto name=utf8(std::filesystem::path(profileFiles[i]).filename().wstring());
                if(ImGui::Selectable(name.c_str(),i==profileIndex))profileIndex=i;}
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(profileFiles.empty());
        if(button("加载")){
            if(!changes()||MessageBoxW(window,AIM_TEXT(L"加载会覆盖未应用的修改，是否继续？"),AIM_TEXT(L"加载配置"),MB_YESNO|MB_ICONQUESTION)==IDYES){
                auto path=profileFiles[profileIndex];submit([path]{return NativeBackend::Load(path);});
            }
        }
        ImGui::SameLine();
        if(button("保存",90,true)){
            if(MessageBoxW(window,AIM_TEXT(L"应用当前参数并覆盖选中的配置档案？"),AIM_TEXT(L"保存配置"),MB_YESNO|MB_ICONQUESTION)==IDYES){
                auto path=profileFiles[profileIndex];auto values=draft();submit([path,values]{return NativeBackend::Save(path,values);});
            }
        }
        ImGui::EndDisabled();ImGui::SameLine();
        if(button("另存为")){
            auto selectedPath=fileDialog(window,true,AIM_TEXT(L"Aim config\0*.ini\0"),L"ini");
            if(!selectedPath.empty()){auto path=wide(selectedPath);auto values=draft();submit([path,values]{return NativeBackend::Save(path,values);});}
        }
        if(button(AIM_TEXT("导入 INI"))){
            auto selectedPath=fileDialog(window,false,AIM_TEXT(L"Aim config\0*.ini\0"),L"ini");
            if(!selectedPath.empty()&&(!changes()||MessageBoxW(window,AIM_TEXT(L"放弃未应用的修改并加载配置？"),AIM_TEXT(L"导入配置"),MB_YESNO|MB_ICONQUESTION)==IDYES)){
                auto path=wide(selectedPath);submit([path]{return NativeBackend::Load(path);});
            }
        }
        ImGui::SameLine();if(button(AIM_TEXT("刷新档案")))nextBackendPoll=0;
        ImGui::TextDisabled(AIM_TEXT("应用时自动保存到 配置 / aim_config.ini"));endCard();
        section(AIM_TEXT("本地 JSON 快照"));
        beginCard(AIM_TEXT("snapshot"));
        if (button(AIM_TEXT("导入 JSON")))
        {
            auto path = fileDialog(window, false, AIM_TEXT(L"JSON snapshot\0*.json\0"), AIM_TEXT(L"json"));
            if (!path.empty())
            {
                std::ifstream file(std::filesystem::path(wide(path)),std::ios::binary);
                std::string content(262145,'\0');
                file.read(content.data(),static_cast<std::streamsize>(content.size()));
                content.resize(static_cast<size_t>(file.gcount()));
                const bool readable=!file.bad() && content.size()<=262144;
                file.clear();
                notify(readable && importSnapshot(content) ? AIM_TEXT("已导入本地界面参数")
                                                       : AIM_TEXT("导入失败：格式、参数键或数值范围不正确"));
            }
        }
        ImGui::SameLine();
        if (button(AIM_TEXT("导出 JSON")))
        {
            auto path = fileDialog(window, true, AIM_TEXT(L"JSON snapshot\0*.json\0"), AIM_TEXT(L"json"));
            if (!path.empty())
            {
                std::ofstream file(wide(path));
                file << snapshot();
                file.flush();
                notify(file ? AIM_TEXT("参数快照已导出") : AIM_TEXT("写入失败"));
            }
        }
        endCard();
        if (button(AIM_TEXT("重置默认参数")))
            ImGui::OpenPopup(AIM_TEXT("reset"));
        if (ImGui::BeginPopupModal(AIM_TEXT("reset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(AIM_TEXT("将重置当前界面参数，已保存档案不受影响。"));
            if (button(AIM_TEXT("确认重置")))
            {
                resetDraft();
                notify(AIM_TEXT("默认参数已写入草稿，点击应用生效"));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button("取消"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    else if (subtab == 1)
    {
        section(AIM_TEXT("本次编辑摘要"));
        if (button(AIM_TEXT("恢复已确认值")))
        {
            fields = applied;
            notify(AIM_TEXT("已恢复上次确认的参数"));
        }
        ImGui::SameLine();
        if (button(AIM_TEXT("应用当前参数"), 130, true))
        {
            applyDraft();
        }
        beginCard(AIM_TEXT("changes"));
        int n = changes();
        static int changePage = 0;
        int pageCount = std::max(1, (n + 7) / 8);
        changePage = std::clamp(changePage, 0, pageCount - 1);
        int changeIndex = 0;
        if (!n)
            ImGui::TextDisabled(AIM_TEXT("尚无参数变更"));
        for (size_t i = 0; i < fields.size(); i++)
            if (!sameField(fields[i],applied[i]))
            {
                if (changeIndex++ / 8 != changePage)
                    continue;
                ImGui::TextUnformatted(fields[i].label.c_str());
                ImGui::SameLine(200 * scale);
                if (fields[i].kind == Text)
                    ImGui::TextDisabled(AIM_TEXT("文本已修改"));
                else
                    ImGui::TextDisabled(AIM_TEXT("%.2f  >  %.2f"), applied[i].value, fields[i].value);
            }
        endCard();
        ImGui::BeginDisabled(changePage == 0);
        if (button("上一页"))
            --changePage;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(AIM_TEXT("%d / %d"), changePage + 1, pageCount);
        ImGui::SameLine();
        ImGui::BeginDisabled(changePage + 1 >= pageCount);
        if (button("下一页"))
            ++changePage;
        ImGui::EndDisabled();
    }
    else
    {
        section(AIM_TEXT("外观与交互"));
        beginCard(AIM_TEXT("appearance"));
        ImGui::TextUnformatted(AIM_TEXT("颜色主题"));
        if (button("深色", 100))
        {
            lightTheme = false;
        }
        ImGui::SameLine();
        if (button("浅色", 100))
        {
            lightTheme = true;
        }
        ImGui::Spacing();
        ImGui::Checkbox(AIM_TEXT("平滑动画"), &motion);
        ImGui::TextUnformatted("强调色");
        const ImVec4 colors[] = {
            {.48f, .66f, .95f, 1}, {.40f, .73f, .72f, 1}, {.69f, .59f, .88f, 1}, {.85f, .63f, .72f, 1}};
        for (int i = 0; i < 4; i++)
        {
            ImGui::PushID(i);
            if (i)
                ImGui::SameLine();
            if (ImGui::ColorButton(AIM_TEXT("##accent"), colors[i], ImGuiColorEditFlags_NoTooltip, {28 * scale, 28 * scale}))
                accent = colors[i];
            ImGui::PopID();
        }
        endCard();
        section(AIM_TEXT("操作提示"));
        ImGui::TextDisabled(AIM_TEXT("F1 - F7 切换页面\nCtrl + T 切换深浅主题\n拖动顶部空白区域移动窗口\n拖动边缘调整大小\nCtrl "
                            "+ 单击滑块可输入精确数值"));
    }
}
void logPage()
{
    static int logPageIndex = 0;
    section(AIM_TEXT("事件日志 · 最近 30 秒"));
    ImGui::SetNextItemWidth(190 * scale);
    if (ImGui::InputTextWithHint(AIM_TEXT("##logsearch"), AIM_TEXT("搜索事件…"), search, sizeof(search)))
        logPageIndex = 0;
    ImGui::SameLine();
    if (button("清空")){events.clear();logClearTime=RecentLog::Clock::now();}
    ImGui::SameLine();
    ImGui::Checkbox(AIM_TEXT("跟随最新"), &followLog);
    std::vector<std::string> filtered;
    for (auto &event : events)
        if (!search[0] || event.text.find(search) != std::string::npos)
            filtered.push_back(event.text);
    int count = std::max(1, ((int)filtered.size() + 5) / 6);
    if (followLog)
        logPageIndex = count - 1;
    logPageIndex = std::clamp(logPageIndex, 0, count - 1);
    beginCard(AIM_TEXT("logs"));
    if (filtered.empty())
        ImGui::TextDisabled(AIM_TEXT("暂无匹配事件"));
    for (int i = logPageIndex * 6; i < std::min((int)filtered.size(), (logPageIndex + 1) * 6); i++)
    {
        ImGui::TextColored(accent, "•");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", filtered[i].c_str());
        ImGui::Spacing();
    }
    endCard();
    ImGui::BeginDisabled(logPageIndex == 0);
    if (button("上一页"))
    {
        followLog = false;
        --logPageIndex;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(AIM_TEXT("%d / %d"), logPageIndex + 1, count);
    ImGui::SameLine();
    ImGui::BeginDisabled(logPageIndex + 1 >= count);
    if (button("下一页"))
    {
        followLog = false;
        ++logPageIndex;
    }
    ImGui::EndDisabled();
}
bool titleButton(const char *id, ImVec2 pos, int kind)
{
    ImGui::SetCursorPos(pos);
    bool clicked = ImGui::InvisibleButton(id, {28 * scale, 28 * scale});
    auto d = ImGui::GetWindowDrawList();
    ImVec2 center = at(pos, 14, 14);
    ImU32 c = col(ImGui::IsItemHovered() ? accent : p.muted);
    if (kind == 0)
    {
        d->AddCircle(center, 5.5f * scale, c, 32, 1.2f * scale);
        for (int i = 0; i < 8; i++)
        {
            float angle = i * .785398f;
            d->AddLine({center.x + cosf(angle) * 8 * scale, center.y + sinf(angle) * 8 * scale},
                       {center.x + cosf(angle) * 10 * scale, center.y + sinf(angle) * 10 * scale}, c, scale);
        }
    }
    else if (kind == 1)
        d->AddLine(at(center, -6, 0), at(center, 6, 0), c, 1.2f * scale);
    else
    {
        d->AddLine(at(center, -5, -5), at(center, 5, 5), c, 1.2f * scale);
        d->AddLine(at(center, 5, -5), at(center, -5, 5), c, 1.2f * scale);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", kind == 0 ? AIM_TEXT("切换深浅主题") : kind == 1 ? "最小化" : "关闭");
    return clicked;
}
void sidebar(float W, float H)
{
    const float x = W - 156, w = 132;
    auto d = ImGui::GetWindowDrawList();
    ImVec2 center{(x + w * .5f) * scale, 134 * scale};
    d->AddCircleFilled(center, 44 * scale, col(p.select));
    if (avatarTexture)
        d->AddImageRounded(ImTextureRef(avatarTexture), at(center, -42, -42), at(center, 42, 42), {0, 0}, {1, 1},
                           col({1, 1, 1, 1}), 42 * scale);
    d->AddCircle(center, 45 * scale, col(p.border), 96, scale);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char time[16], date[64];
    snprintf(time, sizeof(time), AIM_TEXT("%02d:%02d"), now.wHour, now.wMinute);
    const char *weekday[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    snprintf(date, sizeof(date), AIM_TEXT("%04d / %02d / %02d  %s"), now.wYear, now.wMonth, now.wDay, weekday[now.wDayOfWeek]);
    text({x * scale, 195 * scale}, time, 34, p.text);
    text({x * scale, 245 * scale}, date, 9.5f, p.muted);
    text({x * scale, 294 * scale}, AIM_TEXT("系统状态"), 12, p.text);
    const char *labels[] = {AIM_TEXT("采集 FPS"), AIM_TEXT("推理 ms"), AIM_TEXT("内存占用"), AIM_TEXT("GPU 占用"), AIM_TEXT("CPU 占用")};
    char values[5][32]{};const auto& t=backendState.telemetry;const auto& r=backendState.resources;
    const bool fresh=snapshotFresh;
    if(fresh){snprintf(values[0],32,AIM_TEXT("%.0f"),running?t.fps:0);snprintf(values[1],32,AIM_TEXT("%.1f"),running?t.inferMs:0);}
    else {strcpy_s(values[0],"—");strcpy_s(values[1],"—");}
    const double usage[]={r.memoryPercent,r.gpuPercent,r.cpuPercent};
    for(int i=0;i<3;++i){if(fresh&&usage[i]>=0)snprintf(values[i+2],32,AIM_TEXT("%.1f%%"),usage[i]);else strcpy_s(values[i+2],"—");}
    float levels[]={fresh&&running?t.fps/240.f:0,fresh&&running?t.inferMs/50.f:0,
        fresh?float(std::max(0.0,r.memoryPercent)/100):0,fresh?float(std::max(0.0,r.gpuPercent)/100):0,fresh?float(std::max(0.0,r.cpuPercent)/100):0};
    for (int i=0;i<5;++i){
        const float y=326+i*44.f;
        icon({(x+6)*scale,(y+9)*scale},i==3?4:2,p.muted,5);
        text({(x+22)*scale,y*scale},labels[i],10.5f,p.muted);
        const auto tw=regular->CalcTextSizeA(12*scale,FLT_MAX,0,values[i]);
        text({(x+w)*scale-tw.x,y*scale},values[i],10.5f,p.text);
        resourceLevels[i]=std::clamp(levels[i],0.f,1.f);
        d->AddRectFilled({x*scale,(y+27)*scale},{(x+w)*scale,(y+29)*scale},col(p.track),scale);
        if(resourceLevels[i]>.001f)d->AddRectFilled({x*scale,(y+27)*scale},{(x+w*resourceLevels[i])*scale,(y+29)*scale},col(accent),scale);
        if(ImGui::IsMouseHoveringRect({x*scale,y*scale},{(x+w)*scale,(y+32)*scale})){
            if(i==2&&fresh&&r.memoryPercent>=0)ImGui::SetTooltip(AIM_TEXT("整机物理内存：%.2f / %.2f GiB"),r.memoryUsedGiB,r.memoryTotalGiB);
            else if(i==3)ImGui::SetTooltip(AIM_TEXT("整机 GPU 最繁忙引擎利用率；跨进程合计同一引擎。\n— 表示尚未采样、驱动不支持或计数器不可用。"));
            else if(i==4)ImGui::SetTooltip(AIM_TEXT("整机 CPU 总利用率；每秒采样。\n— 表示尚未采样或计数器不可用。"));
        }
    }
    text({x*scale,555*scale},AIM_TEXT("整机资源 · 1 秒更新"),9.5f,p.muted);
}
} // namespace
void Initialize(float uiScale, bool light)
{
    scale = uiScale;
    lightTheme = light;
    themeBlend = light ? 1.f : 0.f;
    defaults();
    applied = fields;
    modelClassesPath.clear(); running=false; connected=false; pageFade=1; navY=114;
    transitionAge=1;transitionDirection=1;requestedSubtab=-1;resetContentScroll=false;tabLineReady.fill(false);navHover.fill(0);resourceLevels.fill(0);
    events.clear(); lastBackendLogId=0;logClearTime={}; profileIndex=0; page=0; subtab=0;
    regular=nullptr; humanTexture=avatarTexture=0; nextBackendPoll=0; backendLoaded=false;
    fpsHistory={}; frozenFps.clear(); freezeFps=false;
    captureDevices.clear(); captureModes.clear(); modelClasses.clear(); toast.clear(); toastLife=0;
    captureRequested=false;modesRequested=false;captureGeneration=0;captureStatus=AIM_TEXT("请选择采集设备");
    snapshotFresh=false;lastSnapshot=0;parameterRevision=0;observedCaptureDevice.clear();observedCaptureBackend=-1;profileFiles.clear();modelFiles.clear();
    snapshotPending=std::async(std::launch::async,[]{return NativeBackend::ReadSnapshot();});

    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    char windows[MAX_PATH]{};
    GetWindowsDirectoryA(windows, MAX_PATH);
    std::string font = std::string(windows) + AIM_TEXT("\\Fonts\\msyh.ttc");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
        regular = io.Fonts->AddFontFromFileTTF(font.c_str(), 16 * scale, &cfg);
    if (!regular)
        regular = io.Fonts->AddFontDefault();
    auto &st = ImGui::GetStyle();
    st.WindowPadding = {0, 0};
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.ChildRounding = 16;
    st.FrameRounding = 9;
    st.PopupRounding = 14;
    st.PopupBorderSize = 1;
    st.FramePadding = {9, 5};
    st.ItemSpacing = {10, 9};
    st.CellPadding = {5, 5};
    st.ScrollbarSize = 4;
    st.ScrollbarRounding = 8;
    st.GrabRounding = 8;
    st.WindowRounding = 0;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;
    st.CircleTessellationMaxError = .30f;
    baseStyle = st;
    st.ScaleAllSizes(scale);
}
void SetHumanTexture(std::uint64_t texture)
{
    humanTexture = texture;
}
void SetAvatarTexture(std::uint64_t texture)
{
    avatarTexture = texture;
}
bool LayoutFits()
{
    return layoutFits;
}
bool CanClose(HWND window) {
    if(actionsBusy()){MessageBoxW(window,AIM_TEXT(L"操作仍在执行，请等待完成后关闭。"),AIM_TEXT(L"VISION"),MB_OK|MB_ICONINFORMATION);return false;}
    return !changes() || MessageBoxW(window,AIM_TEXT(L"还有未应用的参数，放弃这些修改并关闭界面？"),AIM_TEXT(L"VISION"),MB_YESNO|MB_ICONQUESTION)==IDYES;
}
void Shutdown() {
    if(pending.valid()) {try {pending.get();} catch(...) {}}
    if(capturePending.valid()) {try {capturePending.get();} catch(...) {}}
    if(snapshotPending.valid()) {try {snapshotPending.get();} catch(...) {}}
    backendLoaded=false; regular=nullptr; humanTexture=avatarTexture=0;
}
void Tick(bool visible) {
    try { pollBackend(visible); } catch(const std::exception& e) { nextBackendPoll=backendClock()+2; notify(e.what()); }
}
void Render(HWND window)
{
    auto &io = ImGui::GetIO();
    try { pollBackend(GetForegroundWindow()==window); } catch(const std::exception& e) { nextBackendPoll=backendClock()+2; notify(e.what()); }
    // Shrinking the window scales the entire layout rather than clipping labels.
    float newScale = std::min(io.DisplaySize.x / 1020.f, io.DisplaySize.y / 690.f);
    if (newScale > 0 && std::abs(newScale - scale) > .001f)
    {
        ImGui::GetStyle() = baseStyle;
        ImGui::GetStyle().ScaleAllSizes(newScale);
        scale = newScale;
    }
    cardPaddingFits = true;transitionCard=0;
    if(requestedSubtab>=0){startTransition(subtab,requestedSubtab);subtab=requestedSubtab;requestedSubtab=-1;}
    ImGui::PushFont(regular, 16 * scale);
    palette();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T))
        lightTheme = !lightTheme;
    for (int i = 0; i < 7; i++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + i)))
        {
            startTransition(page,i);page = i;
            subtab = 0;tabLineReady[page]=false;
        }
    transitionAge=motion?std::min(1.f,transitionAge+std::min(io.DeltaTime,.05f)):1.f;
    const float progress=std::clamp(transitionAge/.34f,0.f,1.f);
    pageFade=1-std::pow(1-progress,3.f);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(AIM_TEXT("Vision workspace"), nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
    float W = io.DisplaySize.x / scale, H = io.DisplaySize.y / scale;
    float left = 166, right = 178, contentW = W - left - right - 48;
    auto d = ImGui::GetWindowDrawList();
    const float radius = 32 * scale;
    auto shadow = ImGui::GetBackgroundDrawList();
    shadow->PushClipRect({-24, -24}, {io.DisplaySize.x + 24, io.DisplaySize.y + 24}, false);
    // Thin outer rings avoid alpha-blending the whole window twenty times.
    for (int i = 4; i >= 1; --i)
    {
        float spread = static_cast<float>(i * 3);
        shadow->AddRect({-spread, -spread + 2}, {io.DisplaySize.x + spread, io.DisplaySize.y + spread + 2},
                        IM_COL32(0, 0, 0, 18-i*4), radius + spread, 0, 3.f);
    }
    shadow->PopClipRect();
    d->AddRectFilled({0, 0}, io.DisplaySize, col(p.bg), radius);
    d->AddRectFilled({0, 0}, {left * scale, io.DisplaySize.y}, col(p.side), radius, ImDrawFlags_RoundCornersLeft);
    d->AddRect({.5f, .5f}, {io.DisplaySize.x - .5f, io.DisplaySize.y - .5f}, col(p.border, .7f), radius, 0, scale);
    d->AddLine({left * scale, 22 * scale}, {left * scale, (H - 22) * scale}, col(p.border, .5f));
    d->AddLine({(W - right) * scale, 91 * scale}, {(W - right) * scale, (H - 68) * scale}, col(p.border, .7f));
    ImVec2 logo = {31 * scale, 37 * scale};
    d->AddLine(at(logo, -8, 0), at(logo, 8, 0), col(accent), 1.4f * scale);
    d->AddLine(at(logo, 0, -11), at(logo, 0, 11), col(accent), 1.4f * scale);
    d->AddQuadFilled(at(logo, 0, -7), at(logo, 3, 0), at(logo, 0, 7), at(logo, -3, 0), col(accent));
    text({51 * scale, 22 * scale}, AIM_TEXT("V I S I O N"), 16, p.text);
    text({51 * scale, 48 * scale}, AIM_TEXT("简 · 稳 · 从容"), 10, p.muted);
    const char *names[] = {"总览", "自瞄", "控制", "找色", "设备", "配置", "日志"};
    float target = 114 + page * 49.f;
    navY += (target - navY) * (motion ? 1 - std::exp(-io.DeltaTime * 15) : 1);
    d->AddRectFilled({16 * scale, navY * scale}, {150 * scale, (navY + 39) * scale}, col(p.select), 9 * scale);
    d->AddRectFilled({16 * scale, (navY + 12) * scale}, {19 * scale, (navY + 27) * scale}, col(accent), 2 * scale);
    for (int i = 0; i < 7; i++)
    {
        float y = 114 + i * 49.f;
        ImGui::SetCursorPos({16 * scale, y * scale});
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("nav", {134 * scale, 39 * scale}))
        {
            startTransition(page,i);page = i;
            subtab = 0;tabLineReady[page]=false;
        }
        const float hover=ImGui::IsItemHovered()?1.f:0.f;
        navHover[i]+=(hover-navHover[i])*(motion?1-std::exp(-io.DeltaTime*16):1);
        if(i!=page)d->AddRectFilled({16*scale,y*scale},{150*scale,(y+39)*scale},col(p.hover,navHover[i]*.65f),9*scale);
        icon({(36+2*navHover[i]) * scale, (y + 19.5f) * scale}, i, page == i ? accent : p.muted, 7);
        text({59 * scale, (y + 10) * scale}, names[i], 13, page == i ? p.text : p.muted);
        ImGui::PopID();
    }
    d->AddCircleFilled({28 * scale, (H - 49) * scale}, 3 * scale, col(connected ? ImVec4(.49f, .83f, .78f, 1) : p.muted));
    text({40 * scale, (H - 57) * scale}, backendLoaded ? (connected ? AIM_TEXT("设备已连接") : AIM_TEXT("设备未连接")) : AIM_TEXT("参数读取失败"), 11, p.muted);
    text({25 * scale, (H - 31) * scale}, AIM_TEXT("v1.0   /   IMGUI"), 9, p.muted);
    const char *titles[] = {AIM_TEXT("工作总览"), AIM_TEXT("自瞄设置"), AIM_TEXT("控制与追踪"), AIM_TEXT("颜色辅助"), AIM_TEXT("设备与采集"), AIM_TEXT("配置与外观"), AIM_TEXT("事件日志")};
    const char *subtitles[] = {AIM_TEXT("常用功能，一处掌控。"),    AIM_TEXT("识别、筛选与激活。"),     AIM_TEXT("让每一次响应更加平滑。"),
                               AIM_TEXT("更精确的颜色范围。"),      AIM_TEXT("管理输入、输出与模型。"), AIM_TEXT("保留你的使用习惯。"),
                               AIM_TEXT("仅保留最近 30 秒的记录。")};
    text({(left + 24+transitionOffset()*.5f) * scale, 24 * scale}, titles[page], 22, mix(p.muted,p.text,.5f+.5f*pageFade));
    text({(left + 25) * scale, 57 * scale}, subtitles[page], 11, p.muted);
    if (titleButton(AIM_TEXT("theme"), {(W - 119) * scale, 20 * scale}, 0))
        lightTheme = !lightTheme;
    if (titleButton(AIM_TEXT("minimize"), {(W - 80) * scale, 20 * scale}, 1))
        ShowWindow(window, SW_MINIMIZE);
    if (titleButton(AIM_TEXT("close"), {(W - 41) * scale, 20 * scale}, 2))
        PostMessageW(window, WM_CLOSE, 0, 0);
    ImGui::SetCursorPos({(left + 24+transitionOffset()) * scale, 94 * scale});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0, 0, 0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, .55f + .45f * pageFade);
    char contentId[64];
    snprintf(contentId, sizeof(contentId), AIM_TEXT("content-%d"), page);
    if(resetContentScroll){ImGui::SetNextWindowScroll({0,0});resetContentScroll=false;}
    ImGui::BeginChild(contentId, {contentW * scale, (H - 166) * scale}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    ImGui::BeginDisabled((busy() || !backendLoaded) && page!=6);
    switch (page)
    {
    case 0:
        overview();
        break;
    case 1:
        aimPage();
        break;
    case 2:
        motionPage();
        break;
    case 3:
        colorPage();
        break;
    case 4:
        devicePage(window);
        break;
    case 5:
        configPage(window);
        break;
    case 6:
        logPage();
        break;
    }
    ImGui::EndDisabled();
    layoutFits = ImGui::GetCursorPosY() <= (H - 166) * scale + 1 && cardPaddingFits;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    sidebar(W, H);
    d->AddLine({(left + 24) * scale, (H - 64) * scale}, {(W - right - 24) * scale, (H - 64) * scale},
               col(p.border, .6f));
    ImGui::SetCursorPos({(left + 24) * scale, (H - 49) * scale});
    if(busy())ImGui::TextDisabled(AIM_TEXT("正在处理 · %.0f 秒"),backendClock()-pendingSince);
    else ImGui::TextDisabled(!snapshotFresh?AIM_TEXT("后端状态更新中…"):changes()?AIM_TEXT("%d 项待应用"):AIM_TEXT("参数已同步"),changes());
    ImGui::SetCursorPos({(W - right - 118) * scale, (H - 52) * scale});
    ImGui::BeginDisabled(actionsBusy() || !backendLoaded);
    if (button(AIM_TEXT("应用参数"), 94))
    {
        applyDraft();
    }
    ImGui::SetCursorPos({(W - right + 22) * scale, (H - 53) * scale});
    if (button(running ? "暂停" : "开始", right - 44, true)) {
        if (running) requestPause(); else requestStart();
    }
    ImGui::EndDisabled();
    if (toastLife > 0)
    {
        toastLife -= io.DeltaTime;
        ImGui::SetNextWindowPos({(left+24)*scale,(H-108)*scale},ImGuiCond_Always,{0,1});
        ImGui::SetNextWindowSize({contentW*scale,0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*scale,10*scale});
        ImGui::Begin(AIM_TEXT("operation-result"),nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoInputs);
        ImGui::TextWrapped("%s",toast.c_str());
        ImGui::End(); ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopFont();
}
} // namespace AppUI
