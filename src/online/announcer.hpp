// Keeps a game server announced on the master server: tells it when something
// about the game changes, and every ten seconds that the game is still there.
// Used by the game when it hosts and by the dedicated server. Header only, so
// that neither the network library nor the online library depends on the other.
#pragma once

#include <cstdint>
#include <string>

#include "net/server.hpp"
#include "online/online.hpp"

namespace ab::online {

class HostAnnouncer {
public:
    // `listed`: shown in the list of games; otherwise found by its code only.
    void update(std::uint64_t nowMs, Online& online, const net::Server& server, const std::string& name, bool listed) {
        LobbyDesc d;
        d.name = name;
        d.listed = listed;
        const net::LobbyState& lobby = server.lobby();
        d.players = 0;
        d.seats = 0;
        for (const net::Seat& seat : lobby.seats) {
            d.players += seat.kind == net::SeatKind::Human ? 1 : 0;
            d.seats += seat.kind != net::SeatKind::Computer ? 1 : 0;  // taken by people, or free for them
        }
        d.phase = server.phase() == net::Phase::Lobby ? "lobby" : server.phase() == net::Phase::Result ? "result" : "round";
        d.level = lobby.settings.schemeTitle;
        d.identityKey = server.identityKeyHex();
        d.port = server.port();
        if (sentAt_ != 0 && d == last_ && nowMs - sentAt_ < 10000) return;
        if (nowMs - sentAt_ < 1000 && sentAt_ != 0) return;  // changes in quick succession go out together
        online.announce(d);
        last_ = d;
        sentAt_ = nowMs;
    }
    void reset() {
        last_ = LobbyDesc{};
        sentAt_ = 0;
    }

private:
    LobbyDesc last_;
    std::uint64_t sentAt_ = 0;
};

}  // namespace ab::online
