#pragma once
#include "../common/obfuscated_text.h"
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

// Small bounded JSON reader for existing Aim API metadata and local snapshots.
namespace NativeJson {
struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string scalar;
    std::vector<Value> array;
    std::map<std::string, Value> object;
    const Value& at(const std::string& key) const {
        static const Value empty;
        auto it = object.find(key);
        return it == object.end() ? empty : it->second;
    }
    std::string text() const { return scalar; }
};
class Reader {
    const std::string& s;
    size_t pos = 0;
    [[noreturn]] void fail() const { throw std::runtime_error(AIM_TEXT_TEMP("JSON 格式无效")); }
    void ws() { while (pos < s.size() && (s[pos]==' ' || s[pos]=='\r' || s[pos]=='\n' || s[pos]=='\t')) ++pos; }
    bool take(char c) { ws(); if (pos<s.size() && s[pos]==c) {++pos;return true;} return false; }
    unsigned hex() {
        unsigned v=0;
        for(int i=0;i<4;++i) {
            if(pos==s.size()) fail();
            char c=s[pos++]; unsigned n;
            if(c>='0'&&c<='9')n=c-'0'; else if(c>='a'&&c<='f')n=c-'a'+10;
            else if(c>='A'&&c<='F')n=c-'A'+10; else fail();
            v=(v<<4)|n;
        }
        return v;
    }
    static void utf8(std::string& out,unsigned c) {
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
        else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    std::string string() {
        if(!take('"'))fail();
        std::string out;
        while(pos<s.size()) {
            unsigned char c=s[pos++];
            if(c=='"')return out;
            if(c<32)fail();
            if(c!='\\'){out+=char(c);continue;}
            if(pos==s.size())fail();
            switch(s[pos++]) {
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;
            case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{unsigned cp=hex();if(cp>=0xd800&&cp<=0xdbff){
                if(pos+2>s.size()||s[pos++]!='\\'||s[pos++]!='u')fail();
                unsigned low=hex();if(low<0xdc00||low>0xdfff)fail();cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;
            }else if(cp>=0xdc00&&cp<=0xdfff)fail();utf8(out,cp);break;}
            default:fail();
            }
        }
        fail();
    }
    Value value(int depth) {
        if(depth>16)fail();ws();if(pos==s.size())fail();Value v;
        if(s[pos]=='"'){v.type=Value::String;v.scalar=string();return v;}
        if(take('{')) {
            v.type=Value::Object;if(take('}'))return v;
            do{auto key=string();if(!take(':'))fail();auto item=value(depth+1);
                if(!v.object.emplace(key,std::move(item)).second)fail();
                if(take('}'))return v;
            }while(take(','));fail();
        }
        if(take('[')) {
            v.type=Value::Array;if(take(']'))return v;
            do{v.array.push_back(value(depth+1));if(take(']'))return v;}while(take(','));fail();
        }
        for(const char* word:{"true","false","null"}) {
            std::string w=word;if(s.compare(pos,w.size(),w)==0){pos+=w.size();v.type=w=="null"?Value::Null:Value::Bool;v.scalar=w;return v;}
        }
        size_t begin=pos;if(s[pos]=='-')++pos;
        if(pos==s.size())fail();
        if(s[pos]=='0')++pos;
        else {if(s[pos]<'1'||s[pos]>'9')fail();while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;}
        if(pos<s.size()&&s[pos]=='.'){++pos;size_t p=pos;while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;if(p==pos)fail();}
        if(pos<s.size()&&(s[pos]=='e'||s[pos]=='E')){++pos;if(pos<s.size()&&(s[pos]=='+'||s[pos]=='-'))++pos;size_t p=pos;while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;if(p==pos)fail();}
        v.type=Value::Number;v.scalar=s.substr(begin,pos-begin);
        if(!std::isfinite(std::strtod(v.scalar.c_str(),nullptr)))fail();return v;
    }
public:
    explicit Reader(const std::string& source):s(source){}
    Value read(){if(s.size()>262144)fail();auto v=value(0);ws();if(pos!=s.size())fail();return v;}
};
inline Value parse(const std::string& s){return Reader(s).read();}
inline std::string quote(const std::string& s) {
    const char* hex="0123456789abcdef";std::string out="\"";
    for(unsigned char c:s){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;
    case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;
    default:if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}}
    return out+'"';
}
}
