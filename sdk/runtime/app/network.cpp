// mechassault-recomp patch: the PC's network adapters for System Link (Settings::lan_adapter, F1 > Network). A
// virtual LAN (Radmin VPN, ZeroTier, Hamachi, a WireGuard or Tailscale network) is an adapter like any other: chosen
// here, the game's address is that adapter's and its broadcasts reach the other machines on it (xdk/xnet.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <string>
#include <vector>

#include "app/app.h"

#pragma comment(lib, "iphlpapi.lib")

namespace app {

std::vector<NetAdapter> NetworkAdapters() {
    std::vector<NetAdapter> out;
    ULONG size = 16384;
    std::vector<uint8_t> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::string name;
        for (const wchar_t* c = a->FriendlyName; c && *c; c++) name += *c < 0x80 ? char(*c) : '?';
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            uint8_t len = u->OnLinkPrefixLength;
            uint32_t mask = len ? htonl(len >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> len)) : 0;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &ip, text, sizeof(text));
            out.push_back({name, text, ip, ip | ~mask});
        }
    }
    return out;
}

}  // namespace app
