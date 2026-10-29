#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <charconv>
#include <fstream>
#include <string>
#include <thread>
#include <map>
#include "control.h"

namespace spt {
constexpr UINT kBridgeMessage = WM_APP + 12;
struct BridgeRequest {
    bool set = false;
    Settings settings;
    int status = 200;
    std::string json;
};
inline std::atomic<bool> bridgeRunning{false};
inline std::atomic<int> bridgeError{0};
inline std::string lower(std::string text) {
    for(char& c : text) if(c >= 'A' && c <= 'Z') c += 'a'-'A';
    return text;
}
inline bool parseSettings(const std::string& body, Settings& out) {
    std::map<std::string,int> values;
    size_t pos=0;
    while(pos<body.size()) {
        size_t end=body.find('&',pos); if(end==std::string::npos)end=body.size();
        size_t eq=body.find('=',pos); if(eq==std::string::npos || eq>=end)return false;
        std::string key=body.substr(pos,eq-pos);
        int value=0; auto r=std::from_chars(body.data()+eq+1,body.data()+end,value);
        if(r.ec!=std::errc{} || r.ptr!=body.data()+end || !values.emplace(key,value).second)return false;
        pos=end+1;
    }
    if(values.size()!=7 || !values.count("tempo") || !values.count("pitch") || !values.count("linked") || !values.count("enabled") ||
        !values.count("reverb") || !values.count("distortion") || !values.count("muffle")) return false;
    out={values["tempo"],values["pitch"],values["linked"],values["enabled"],values["reverb"],values["distortion"],values["muffle"]};
    return validSettings(out);
}
inline void startBridge(HWND owner, const std::wstring& tokenPath) {
    std::ifstream file(tokenPath); std::string token; file>>token;
    if(token.size()!=64 || token.find_first_not_of("0123456789abcdef")!=std::string::npos) {bridgeError=1;return;}
    std::thread([owner,token]() {
        WSADATA data;
        if(WSAStartup(MAKEWORD(2,2),&data)) {bridgeError=2;return;}
        SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(listener==INVALID_SOCKET) {bridgeError=3;WSACleanup();return;}
        BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive));
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(43827);local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(listener,reinterpret_cast<sockaddr*>(&local),sizeof(local)) || listen(listener,4)) {
            bridgeError=WSAGetLastError();closesocket(listener);WSACleanup();return;
        }
        bridgeRunning=true;
        for(;;) {
            SOCKET client=accept(listener,nullptr,nullptr);
            if(client==INVALID_SOCKET)break;
            DWORD timeout=1500;
            setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            std::string request;char chunk[2048];size_t headerEnd=std::string::npos;
            while(request.size()<8192 && headerEnd==std::string::npos) {
                int n=recv(client,chunk,sizeof(chunk),0);if(n<=0)break;
                request.append(chunk,n);headerEnd=request.find("\r\n\r\n");
            }
            int code=400;std::string body="{\"error\":\"Invalid request\"}";std::string origin;
            if(headerEnd!=std::string::npos && headerEnd<=4096) {
                size_t firstEnd=request.find("\r\n");
                std::string first=request.substr(0,firstEnd);
                size_t space=first.find(' '),second=first.find(' ',space+1);
                std::string method=first.substr(0,space),path=first.substr(space+1,second-space-1);
                std::map<std::string,std::string> headers;
                bool valid=space!=std::string::npos && second!=std::string::npos && first.substr(second+1)=="HTTP/1.1";
                for(size_t pos=firstEnd+2;pos<headerEnd;) {
                    size_t end=request.find("\r\n",pos),colon=request.find(':',pos);
                    if(end==std::string::npos || colon>=end) {valid=false;break;}
                    std::string key=lower(request.substr(pos,colon-pos));size_t valueStart=colon+1;
                    while(valueStart<end && request[valueStart]==' ')++valueStart;
                    if(!headers.emplace(key,request.substr(valueStart,end-valueStart)).second) {valid=false;break;}
                    pos=end+2;
                }
                origin=headers["origin"];
                bool originOK=origin.empty() || origin=="https://xpui.app.spotify.com" || origin=="https://open.spotify.com" || origin=="sp://xpui.app.spotify.com";
                bool hostOK=headers["host"]=="127.0.0.1:43827";
                if(!originOK || !hostOK) {code=403;body="{\"error\":\"Origin or host rejected\"}";origin.clear();}
                else if(valid && method=="OPTIONS") {code=204;body.clear();}
                else if(headers["x-spt-token"]!=token) {code=403;body="{\"error\":\"Invalid token\"}";}
                else if(valid && method=="GET" && path=="/status") {
                    BridgeRequest call;SendMessageW(owner,kBridgeMessage,0,reinterpret_cast<LPARAM>(&call));code=call.status;body=call.json;
                } else if(valid && method=="POST" && path=="/config" && !headers.count("transfer-encoding")) {
                    std::string lengthText=headers["content-length"];size_t length=0;
                    auto parsed=std::from_chars(lengthText.data(),lengthText.data()+lengthText.size(),length);
                    if(parsed.ec==std::errc{} && parsed.ptr==lengthText.data()+lengthText.size() && length<=256) {
                        size_t required=headerEnd+4+length;
                        while(request.size()<required) {
                            int n=recv(client,chunk,int(std::min<size_t>(sizeof(chunk),required-request.size())),0);
                            if(n<=0)break;request.append(chunk,n);
                        }
                        BridgeRequest call;call.set=true;
                        if(request.size()>=required && parseSettings(request.substr(headerEnd+4,length),call.settings)) {
                            SendMessageW(owner,kBridgeMessage,0,reinterpret_cast<LPARAM>(&call));code=call.status;body=call.json;
                        }
                    }
                } else if(valid) {code=404;body="{\"error\":\"Not found\"}";}
            }
            std::string response="HTTP/1.1 "+std::to_string(code)+" "+(code<300?"OK":"Error")+"\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n";
            if(!origin.empty()) response+="Access-Control-Allow-Origin: "+origin+"\r\nVary: Origin\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: X-SPT-Token, Content-Type\r\nAccess-Control-Allow-Private-Network: true\r\n";
            response+="Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
            for(size_t sent=0;sent<response.size();) {
                int n=send(client,response.data()+sent,int(response.size()-sent),0);if(n<=0)break;sent+=n;
            }
            shutdown(client,SD_BOTH);closesocket(client);
        }
        bridgeRunning=false;closesocket(listener);WSACleanup();
    }).detach();
}
}
