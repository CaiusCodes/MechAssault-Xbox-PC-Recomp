// mechassault-recomp patch: System Link games over the internet, found through a game list service (tools/lobby).
// The game's own System Link screens do everything: a game hosted there is listed online, and a System Link search
// also asks every listed host, so internet games show up in the same list as LAN ones. The list service only lists;
// the matches run between the players' PCs (xnet.cpp), the host's router opened through UPnP.
#pragma once

#include <cstdint>
#include <vector>

namespace online {

struct Peer {
    uint32_t address;  // IPv4, network order
    uint16_t port;     // network order
};

// Settings::online_games is on and there is a list service to use
bool Enabled();
// the System Link session this PC hosts began or ended (or its game bound another port); port (network order) is
// the one it is listed with, ports every UDP port its game has bound (opened on the router while it hosts); local is
// this copy's LAN address, which a list service on this PC (tools/lobby/local_lobby.py, for tests with --instance
// copies) lists instead of the public one
void Hosting(bool on, uint32_t local, uint16_t port, const std::vector<uint16_t>& ports);
// the listed games a System Link search (a broadcast) should also reach, at the port it is broadcast to: the last
// list fetched (searching keeps it fresh), not this PC's own; none while this PC hosts (its broadcasts are its
// answers to searches). A host's router forwards each of its game's ports to the same port, so one listed port
// stands for them all.
std::vector<Peer> Hosts();
// this PC's public IPv4 address as the list service sees it, network order; 0 while unknown
uint32_t PublicAddress();
// at startup: ask the list service once, so the public address is known by the time System Link needs it
void Prepare();

}  // namespace online
