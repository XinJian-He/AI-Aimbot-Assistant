#pragma once
#include <string>
#include <cwctype>
namespace fg {
// MF and DirectShow use the same PnP link with different optional prefixes.
// Never deduplicate by friendly name: two identical cards may share that name.
inline std::wstring CanonicalDeviceId(std::wstring id) {
    for(auto& c:id)c=static_cast<wchar_t>(std::towlower(c));
    const std::wstring prefix=L"@device:pnp:";
    if(id.rfind(prefix,0)==0)id.erase(0,prefix.size());
    if(id.rfind(L"\\??\\",0)==0)id.replace(0,4,L"\\\\?\\");
    return id;
}
inline bool SameDeviceId(const std::wstring& a,const std::wstring& b){
    return !a.empty()&&!b.empty()&&CanonicalDeviceId(a)==CanonicalDeviceId(b);
}
}
