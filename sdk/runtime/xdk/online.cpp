// mechassault-recomp patch: the game list client (online.h). One background thread talks to the list service over
// HTTPS (WinHTTP) and opens the host's ports on the router (the Windows UPnP NAT API), so the game's threads never
// wait on the network: a host checks in every kBeatMs while it hosts and is taken off when it stops; a search fetches
// the list at most every kListMs while the game keeps searching; a host's check-in answers with the PCs
// searching, which its game's announcements also go to.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <natupnp.h>
#include <oleauto.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "xdk/online.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "oleaut32.lib")

namespace online {
namespace {

// the public list service (tools/lobby/README.md); Settings::lobby_url replaces it, empty here until it is deployed
constexpr const char* kDefaultLobby = "";
constexpr const char* kGame = "ma1pc";
constexpr const char* kVersion = "1";  // games of other versions are not listed to each other
constexpr int kBeatMs = 5000;
constexpr int kListMs = 4000;
constexpr int kSearchMs = 6000;   // a search counts as going on this long after its last broadcast
constexpr int kRouterMs = 60000;  // the router's ports close this long after the last hosting or searching

using Clock = std::chrono::steady_clock;

std::mutex g_lock;
std::condition_variable g_wake;
bool g_started = false;
// wanted state, set by the game's threads
bool g_hosting = false;
uint32_t g_local = 0;
uint16_t g_port = 0, g_search_port = 0;
std::vector<uint16_t> g_ports;
Clock::time_point g_searched{};
// what the thread has done
std::vector<Peer> g_hosts, g_searchers;
std::string g_listed;  // the addr this PC is listed under, empty when it is not
std::atomic<uint32_t> g_public{0};

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}

std::string Url() {
    std::lock_guard<std::recursive_mutex> ui(app::UiLock());
    const std::string& u = app::Get().lobby_url;
    return u.empty() ? kDefaultLobby : u;
}

bool LocalLobby(const std::string& url) { return url.find("://127.") != std::string::npos; }

// one request; the body, or empty on any failure (logged once in a while)
std::string Request(const std::string& url, const char* method, const std::string& path) {
    std::wstring full(url.begin(), url.end());
    full += std::wstring(path.begin(), path.end());
    URL_COMPONENTS c{sizeof(c)};
    wchar_t host[256] = {}, rest[1024] = {};
    c.lpszHostName = host;
    c.dwHostNameLength = 256;
    c.lpszUrlPath = rest;
    c.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(full.c_str(), 0, 0, &c)) return {};
    std::string body;
    HINTERNET s = WinHttpOpen(L"MechAssault-PC/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET h = s ? WinHttpConnect(s, host, c.nPort, 0) : nullptr;
    std::wstring verb(method, method + std::strlen(method));
    HINTERNET r = h ? WinHttpOpenRequest(h, verb.c_str(), rest, nullptr, nullptr, nullptr,
                                         c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (s) WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    DWORD status = 0, size = sizeof(status);
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr)) {
        char buf[4096];
        DWORD n = 0;
        while (WinHttpReadData(r, buf, sizeof(buf), &n) && n) body.append(buf, n);
        if (status != 200) {
            LOG_FIRST(20, "online: %s %s: %lu %s", method, path.c_str(), status, body.c_str());
            body.clear();
        }
    } else {
        LOG_FIRST(20, "online: %s %s%s failed (%lu)", method, url.c_str(), path.c_str(), GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (h) WinHttpCloseHandle(h);
    if (s) WinHttpCloseHandle(s);
    return body;
}

// "1.2.3.4" (or "1.2.3.4:5") into a network order address and port
bool Parse(const std::string& text, Peer& p) {
    unsigned b[4], port = 0;
    int n = std::sscanf(text.c_str(), "%u.%u.%u.%u:%u", &b[0], &b[1], &b[2], &b[3], &port);
    if (n < 4 || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255 || port > 65535) return false;
    p.address = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    p.port = htons(uint16_t(port));
    return true;
}

// "you 1.2.3.4" or "ok 1.2.3.4": this PC's public IPv4 address, when the service saw one
void NotePublic(const std::string& word) {
    Peer p{};
    if (!Parse(word, p)) return;
    if (g_public.exchange(p.address) != p.address) LOGI("online: this PC is %s on the internet", Ip(p.address).c_str());
}

// this PC's public IPv4 address from a public STUN server (RFC 5389 binding request), for when the list service saw
// it over IPv6; 0 when none answers
uint32_t Stun() {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("stun.l.google.com", "19302", &hints, &res) != 0 || !res) return 0;
    sockaddr_in server = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    DWORD timeout = 2000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};  // binding request, magic cookie
    for (int i = 8; i < 20; i++) req[i] = uint8_t(std::rand());
    uint32_t found = 0;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        ::sendto(s, reinterpret_cast<const char*>(req), sizeof(req), 0, reinterpret_cast<sockaddr*>(&server), sizeof(server));
        uint8_t buf[512];
        int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 || std::memcmp(buf + 4, req + 4, 16) != 0) continue;
        for (int at = 20; at + 4 <= n;) {
            int type = buf[at] << 8 | buf[at + 1], len = buf[at + 2] << 8 | buf[at + 3];
            if (at + 4 + len > n) break;
            const uint8_t* v = buf + at + 4;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {  // (XOR-)MAPPED-ADDRESS, IPv4
                uint8_t ip[4] = {v[4], v[5], v[6], v[7]};
                if (type == 0x0020)
                    for (int i = 0; i < 4; i++) ip[i] ^= req[4 + i];
                found = ip[0] | ip[1] << 8 | ip[2] << 16 | uint32_t(ip[3]) << 24;
                if (type == 0x0020) break;
            }
            at += 4 + ((len + 3) & ~3);
        }
    }
    ::closesocket(s);
    return found;
}

// the router's ports for the host's game, opened or closed (UPnP); the router keeps them until they are closed
void RouterPorts(const std::vector<uint16_t>& ports, uint32_t local, bool open) {
    if (ports.empty()) return;
    IUPnPNAT* nat = nullptr;
    IStaticPortMappingCollection* maps = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat))) ||
        FAILED(nat->get_StaticPortMappingCollection(&maps)) || !maps) {
        if (open) LOGW("online: no UPnP router found; players outside your network reach your games only if UDP %u "
                       "and up are forwarded to this PC", ntohs(ports.front()));
        if (nat) nat->Release();
        return;
    }
    std::string ip = Ip(local);
    std::wstring me(ip.begin(), ip.end());
    BSTR proto = SysAllocString(L"UDP"), client = SysAllocString(me.c_str()), name = SysAllocString(L"MechAssault PC");
    for (uint16_t p : ports) {
        long port = ntohs(p);
        if (open) {
            IStaticPortMapping* m = nullptr;
            HRESULT hr = maps->Add(port, proto, port, client, VARIANT_TRUE, name, &m);
            if (m) m->Release();
            LOGI("online: router port UDP %ld to %s: %s", port, Ip(local).c_str(), SUCCEEDED(hr) ? "open" : "refused");
        } else {
            maps->Remove(port, proto);
        }
    }
    SysFreeString(proto);
    SysFreeString(client);
    SysFreeString(name);
    maps->Release();
    nat->Release();
}

void Thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Clock::time_point beat{}, listed{}, asked{}, active_at{};
    std::vector<uint16_t> opened;
    uint32_t opened_for = 0;
    std::unique_lock<std::mutex> lock(g_lock);
    for (;;) {
        g_wake.wait_for(lock, std::chrono::milliseconds(500));
        auto now = Clock::now();
        bool hosting = g_hosting && Enabled();
        bool searching = now - g_searched < std::chrono::milliseconds(kSearchMs) && Enabled();
        uint32_t local = g_local;
        uint16_t port = g_port, search_port = g_search_port;
        std::vector<uint16_t> ports = g_ports;
        std::string url = Url();
        lock.unlock();
        bool local_lobby = LocalLobby(url);
        if (hosting || searching) active_at = now;
        // the router first, so this PC answers by the time anyone finds it: a host's announcements and a searcher's
        // replies both come back to the game's own ports
        if ((hosting || searching) && !local_lobby) {
            std::vector<uint16_t> more;
            for (uint16_t p : ports)
                if (std::find(opened.begin(), opened.end(), p) == opened.end()) more.push_back(p);
            RouterPorts(more, local, true);
            opened.insert(opened.end(), more.begin(), more.end());
            opened_for = local;
        } else if (!opened.empty() && now - active_at > std::chrono::milliseconds(kRouterMs)) {
            RouterPorts(opened, opened_for, false);
            opened.clear();
        }
        std::string listed_as;
        {
            std::lock_guard<std::mutex> l(g_lock);
            listed_as = g_listed;
        }
        // this PC's public address, which hosting lists and every XNADDR carries (xnet.cpp OwnXnAddr): from the list
        // service's answers, or a STUN server when the service saw this PC over IPv6; asked again a beat later
        if ((hosting || searching) && !local_lobby && !g_public.load() &&
            now - asked > std::chrono::milliseconds(kBeatMs)) {
            asked = now;
            std::string body = Request(url, "GET", std::string("/v1/list?game=") + kGame + "&ver=" + kVersion);
            if (body.rfind("you ", 0) == 0) NotePublic(body.substr(4, body.find('\n') - 4));
            if (!g_public.load())
                if (uint32_t a = Stun()) NotePublic(Ip(a));
        }
        uint32_t address = local_lobby ? local : g_public.load();
        auto at = [&](uint16_t p) { return address ? Ip(address) + ":" + std::to_string(ntohs(p)) : std::string(); };
        std::string addr = at(port);
        std::vector<Peer> searchers;
        bool beat_done = false;
        if (hosting && address && (addr != listed_as || now - beat > std::chrono::milliseconds(kBeatMs))) {
            std::istringstream in(Request(url, "POST", std::string("/v1/host?game=") + kGame + "&ver=" + kVersion +
                                                           "&addr=" + addr));
            beat = now;
            std::string word, value;
            while (in >> word >> value) {
                Peer p{};
                if (word == "ok") {
                    if (addr != listed_as) LOGI("online: your game is listed as %s", addr.c_str());
                    listed_as = addr;
                } else if (word == "searcher" && Parse(value, p)) {
                    searchers.push_back(p);
                }
            }
            beat_done = true;
        }
        if (!listed_as.empty() && (!hosting || addr != listed_as)) {
            Request(url, "POST", std::string("/v1/unhost?game=") + kGame + "&ver=" + kVersion + "&addr=" + listed_as);
            LOGI("online: your game %s is no longer listed", listed_as.c_str());
            listed_as.clear();
        }
        std::vector<Peer> hosts;
        bool got = false;
        if (searching && !hosting && now - listed > std::chrono::milliseconds(kListMs)) {
            listed = now;
            std::string me = search_port ? at(search_port) : "";
            std::istringstream in(Request(url, "GET", std::string("/v1/list?game=") + kGame + "&ver=" + kVersion +
                                                          (me.empty() ? "" : "&addr=" + me)));
            std::string word, value;
            while (in >> word >> value) {
                Peer p{};
                if (word == "you") NotePublic(value);
                else if (word == "host" && Parse(value, p) && value != listed_as) hosts.push_back(p);
            }
            got = true;
        }
        lock.lock();
        g_listed = listed_as;
        if (got) {
            if (hosts.size() != g_hosts.size()) LOGI("online: %zu games listed", hosts.size());
            g_hosts = hosts;
        }
        if (beat_done) {
            if (searchers.size() != g_searchers.size()) LOGI("online: %zu players searching", searchers.size());
            g_searchers = searchers;
        }
        if (!hosting) g_searchers.clear();
    }
}

void Start() {
    if (g_started) return;
    g_started = true;
    std::thread(Thread).detach();
}

}  // namespace

bool Enabled() {
    if (Url().empty()) return false;
    std::lock_guard<std::recursive_mutex> ui(app::UiLock());
    return app::Get().online_games;
}

void Ports(uint32_t local, const std::vector<uint16_t>& ports) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_local = local;
    g_ports = ports;
}

void Hosting(bool on, uint16_t port) {
    if (on && !Enabled()) return;
    std::lock_guard<std::mutex> lock(g_lock);
    if (on == g_hosting && (!on || port == g_port)) return;
    g_hosting = on;
    if (on) g_port = port;
    Start();
    g_wake.notify_all();
}

std::vector<Peer> Hosts(uint16_t port) {
    if (!Enabled()) return {};
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_hosting) return {};
    bool fresh = Clock::now() - g_searched > std::chrono::milliseconds(kSearchMs);
    g_searched = Clock::now();
    g_search_port = port;
    Start();
    if (fresh) g_wake.notify_all();
    return g_hosts;
}

std::vector<Peer> Searchers() {
    if (!Enabled()) return {};
    std::lock_guard<std::mutex> lock(g_lock);
    return g_hosting ? g_searchers : std::vector<Peer>{};
}

uint32_t PublicAddress() { return g_public.load(); }

void Prepare() {
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lock(g_lock);
    g_searched = Clock::now();
    Start();
    g_wake.notify_all();
}

}  // namespace online
