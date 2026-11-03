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
// the UDP ports (network order) this copy's game has bound, and its LAN address: opened on the router while it hosts or
// searches; a list service on this PC (tools/lobby/local_lobby.py, for tests with --instance copies) lists the LAN
// address instead of the public one
void Ports(uint32_t local, const std::vector<uint16_t>& ports);
// the System Link session this PC hosts began or ended; port (network order) is the one it is listed with
void Hosting(bool on, uint16_t port);
// a System Link search broadcast to this port (network order): the listed games it should also reach, from the last
// list fetched (searching keeps it fresh and lists this PC as searching, at that port); none while this PC hosts (its
// broadcasts are its game's announcements)
std::vector<Peer> Hosts(uint16_t port);
// while this PC hosts: the PCs searching online that its game's announcements should also reach (MechAssault's host
// announces its game by broadcast and does not answer searches, so a searcher past the LAN has to be told of)
std::vector<Peer> Searchers();
// this PC's public IPv4 address as the list service sees it, network order; 0 while unknown
uint32_t PublicAddress();
// at startup: ask the list service once, so the public address is known by the time System Link needs it
void Prepare();

}  // namespace online
