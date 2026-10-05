#include "app/net_ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "app/names.hpp"

namespace ab {

namespace {

using State = net::Client::State;

constexpr int kJoinFields = 3;   // name, address, password; then the servers found
constexpr int kHostRows = 5;     // name, server name, port, password, start
constexpr int kLobbyRows = 15;   // start, then the fourteen settings
constexpr std::uint64_t kChatShownMs = 8000;

const char* const kYesNo[2] = {"No", "Yes"};

std::string optionText(const net::MatchSettings& s, int option) {
    static const char* const kSpeed[3] = {"Low", "Medium", "High"};
    static const char* const kDepth[4] = {"None", "A Little", "A Lot", "All the way!"};
    switch (static_cast<net::Option>(option)) {
        case net::Option::Level: return s.level < 0 ? "Random Each Game" : kLevelName[std::clamp(s.level, 0, 10)];
        case net::Option::Scheme: return "Scheme: " + s.schemeTitle;
        case net::Option::Wins: return std::to_string(s.winsNeeded) + (s.winByKills ? " Kills" : " Wins") + " to win match";
        case net::Option::TeamPlay: return std::string("Team Play: ") + kYesNo[s.teamPlay];
        case net::Option::PlayTime: return "Play Time: " + Settings::playTimeText(s.playTime);
        case net::Option::Enclosement: return std::string("Enclosement Depth: ") + kDepth[std::clamp(s.enclosementDepth, 0, 3)];
        case net::Option::Computers: return "Computer Players: " + std::to_string(s.computers);
        case net::Option::RandomStart: return std::string("Random Start: ") + kYesNo[s.randomStart];
        case net::Option::ConveyorSpeed: return std::string("Conveyor Speed: ") + kSpeed[std::clamp(s.conveyorSpeed, 0, 2)];
        case net::Option::StompedBombs: return std::string("Stomped Bombs Detonate: ") + kYesNo[s.stompedBombsDetonate];
        case net::Option::WinByKills: return std::string("Win Matches By Kill Total: ") + kYesNo[s.winByKills];
        case net::Option::DiseasesDestroyable: return std::string("Diseases Can Be Destroyed: ") + kYesNo[s.diseasesDestroyable];
        case net::Option::Goldman: return std::string("Gold Bomberman: ") + kYesNo[s.goldman];
        case net::Option::Campaign: return "Campaign: " + (s.campaign > 0 ? s.campaignTitle : std::string("Off"));
        case net::Option::Count: break;
    }
    return {};
}

}  // namespace

NetUi::NetUi(Settings& settings, std::string gameDir, std::string userSchemesDir, Hooks hooks)
    : cfg_(settings), gameDir_(std::move(gameDir)), userSchemesDir_(std::move(userSchemesDir)), hooks_(std::move(hooks)) {}

NetUi::~NetUi() { close(); }

std::string NetUi::playerName() const {
    std::string name = net::cleanText(cfg_.netName, net::kMaxName);
    if (name.empty()) {
        for (const char* var : {"USER", "USERNAME"})
            if (const char* value = std::getenv(var); name.empty() && value != nullptr) name = net::cleanText(value, net::kMaxName);
    }
    return name.empty() ? "Player" : name;
}

void NetUi::openJoin() {
    close();
    mode_ = entry_ = Mode::Join;
    row_ = 1;
    if (cfg_.netName.empty()) cfg_.netName = playerName();
}

void NetUi::openHost() {
    close();
    mode_ = entry_ = Mode::Host;
    row_ = kHostRows - 1;
    if (cfg_.netName.empty()) cfg_.netName = playerName();
    if (cfg_.netServerName.empty()) cfg_.netServerName = net::cleanText(playerName() + "'s game", 32);
    portText_ = std::to_string(cfg_.netPort);
}

void NetUi::hostNow(int port, bool startMatch) {
    openHost();
    portText_ = std::to_string(port);
    autoStart_ = startMatch;
    startHost();
}

void NetUi::joinNow(const std::string& address) {
    openJoin();
    connectTo(address, false);
}

void NetUi::close() {
    client_.disconnect();
    if (hosting_) server_.stop();
    hosting_ = false;
    browser_.stop();
    editing_ = nullptr;
    chatText_.clear();
    chatOpen_ = false;
    escapeAt_ = 0;
    lastState_ = State::Idle;
    mode_ = Mode::Closed;
}

// Back to the screen the session was started from.
void NetUi::leave() {
    const Mode back = entry_;
    close();
    mode_ = entry_ = back;
    row_ = back == Mode::Host ? kHostRows - 1 : 1;
    if (hooks_.lobbyEntered) hooks_.lobbyEntered();
}

bool NetUi::wantsTextInput() const {
    if (editing_ != nullptr) return true;
    return mode_ == Mode::Session && (client_.state() == State::Lobby || chatOpen_);
}

bool NetUi::showingResult() const { return mode_ == Mode::Session && client_.state() == State::Result; }

bool NetUi::inRound() const { return mode_ == Mode::Session && client_.state() == State::Round; }

void NetUi::edit(std::string* value, std::size_t maxLength, bool digitsOnly) {
    editing_ = value;
    editMax_ = maxLength;
    editDigits_ = digitsOnly;
}

void NetUi::connectTo(const std::string& address, bool remember) {
    if (remember) {
        cfg_.netAddress = address;
        if (hooks_.settingsChanged) hooks_.settingsChanged();
    }
    connectingTo_ = address;
    client_.setPrediction(cfg_.netPrediction);
    client_.connect(address, playerName(), password_, net::clockMs());
    lastState_ = State::Connecting;
    lastRound_ = 0;
    mode_ = Mode::Connecting;
}

void NetUi::startHost() {
    const int port = std::atoi(portText_.c_str());
    if (port < 1 || port > 65535) {
        error_ = "The port must be a number from 1 to 65535";
        mode_ = Mode::Error;
        return;
    }
    cfg_.netPort = port;
    if (hooks_.settingsChanged) hooks_.settingsChanged();
    net::ServerConfig config;
    config.name = net::cleanText(cfg_.netServerName, 32);
    if (config.name.empty()) config.name = playerName() + "'s game";
    config.port = static_cast<std::uint16_t>(port);
    config.password = password_;
    config.gameDir = gameDir_;
    config.userSchemesDir = userSchemesDir_;
    config.scheme = cfg_.scheme;
    // The lobby starts with the settings of the local game.
    config.settings.level = cfg_.level;
    config.settings.winsNeeded = std::clamp(cfg_.winsNeeded, 1, 9);
    config.settings.teamPlay = cfg_.teamPlay;
    config.settings.playTime = cfg_.playTime;
    config.settings.enclosementDepth = cfg_.enclosementDepth;
    config.settings.randomStart = cfg_.randomStart;
    config.settings.conveyorSpeed = cfg_.conveyorSpeed;
    config.settings.stompedBombsDetonate = cfg_.stompedBombsDetonate;
    config.settings.winByKills = cfg_.winByKills;
    config.settings.diseasesDestroyable = cfg_.diseasesDestroyable;
    config.settings.computers = 1;
    config.upnp = true;  // a hosted game tries to open the router's port; the lobby says how it went
    config.log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };
    if (!server_.start(config)) {
        error_ = server_.error() + " (is a server already running on it?)";
        mode_ = Mode::Error;
        return;
    }
    hosting_ = true;
    connectTo("127.0.0.1:" + std::to_string(server_.port()), false);
}

bool NetUi::escape(std::uint64_t nowMs) {
    if (escapeAt_ != 0 && nowMs - escapeAt_ < 3000) return true;
    escapeAt_ = nowMs;
    return false;
}

void NetUi::submitChat() {
    const std::string text = net::cleanText(chatText_, net::kMaxChat);
    chatText_.clear();
    if (text.empty()) return;
    // "/kick NAME": the administrator removes a player.
    if (text.rfind("/kick ", 0) == 0) {
        const std::string who = text.substr(6);
        for (const net::ClientInfo& c : client_.lobby().clients)
            if (c.name == who && c.id != client_.id()) client_.sendKick(c.id);
        return;
    }
    client_.sendChat(text);
}

void NetUi::text(const char* utf8) {
    std::string* target = editing_;
    std::size_t limit = editMax_;
    bool digits = editDigits_;
    if (target == nullptr && wantsTextInput()) {
        target = &chatText_;
        limit = net::kMaxChat;
        digits = false;
    }
    if (target == nullptr) return;
    for (const char* p = utf8; *p != '\0'; ++p) {
        const auto ch = static_cast<unsigned char>(*p);
        // The game's font has the printable ASCII characters only.
        if (ch < 32 || ch > 126 || target->size() >= limit) continue;
        if (digits && (ch < '0' || ch > '9')) continue;
        target->push_back(static_cast<char>(ch));
    }
}

void NetUi::key(unsigned key, bool ctrl) {
    auto sound = [&](int id) {
        if (hooks_.sound) hooks_.sound(id);
    };
    const std::uint64_t now = net::clockMs();
    if (key != SDLK_ESCAPE) escapeAt_ = 0;

    if (mode_ == Mode::Error) {
        if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE) {
            mode_ = entry_;
            row_ = entry_ == Mode::Host ? kHostRows - 1 : 1;
        }
        return;
    }
    if (mode_ == Mode::Connecting) {
        if (key == SDLK_ESCAPE) leave();
        return;
    }
    if (mode_ == Mode::Join || mode_ == Mode::Host) {
        if (editing_ != nullptr) {
            if (key == SDLK_BACKSPACE && !editing_->empty()) editing_->pop_back();
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                const bool wasAddress = editing_ == &cfg_.netAddress;
                editing_ = nullptr;
                cfg_.netName = net::cleanText(cfg_.netName, net::kMaxName);
                if (hooks_.settingsChanged) hooks_.settingsChanged();
                // Enter on the address joins at once.
                if (wasAddress && key != SDLK_ESCAPE && !net::cleanText(cfg_.netAddress, 64).empty()) {
                    sound(10);
                    connectTo(net::cleanText(cfg_.netAddress, 64), true);
                }
            }
            return;
        }
        const int servers = mode_ == Mode::Join ? static_cast<int>(browser_.servers().size()) : 0;
        const int rows = mode_ == Mode::Join ? kJoinFields + servers : kHostRows;
        if (key == SDLK_UP) row_ = (row_ + rows - 1) % rows, sound(20);
        if (key == SDLK_DOWN) row_ = (row_ + 1) % rows, sound(20);
        if (key == SDLK_ESCAPE) close();
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            sound(10);
            if (mode_ == Mode::Join) {
                if (row_ == 0) edit(&cfg_.netName, net::kMaxName, false);
                else if (row_ == 1) edit(&cfg_.netAddress, 64, false);
                else if (row_ == 2) edit(&password_, 32, false);
                else if (row_ - kJoinFields < servers) connectTo(browser_.servers()[static_cast<std::size_t>(row_ - kJoinFields)].address.text(), false);
            } else {
                if (row_ == 0) edit(&cfg_.netName, net::kMaxName, false);
                else if (row_ == 1) edit(&cfg_.netServerName, 32, false);
                else if (row_ == 2) edit(&portText_, 5, true);
                else if (row_ == 3) edit(&password_, 32, false);
                else startHost();
            }
        }
        return;
    }
    if (mode_ != Mode::Session) return;

    const State state = client_.state();
    if (state == State::Lobby) {
        const bool admin = client_.isAdmin() && client_.lobby().phase == net::Phase::Lobby;
        if (key == SDLK_BACKSPACE && !chatText_.empty()) chatText_.pop_back();
        if (key == SDLK_UP) lobbyRow_ = (lobbyRow_ + kLobbyRows - 1) % kLobbyRows, sound(20);
        if (key == SDLK_DOWN) lobbyRow_ = (lobbyRow_ + 1) % kLobbyRows, sound(20);
        if ((key == SDLK_LEFT || key == SDLK_RIGHT) && lobbyRow_ >= 1) {
            if (admin) client_.sendOption(static_cast<net::Option>(lobbyRow_ - 1), key == SDLK_LEFT ? -1 : 1);
            sound(admin ? 20 : 40);
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (!chatText_.empty()) {
                submitChat();
            } else if (lobbyRow_ == 0 && admin) {
                sound(10);
                client_.sendStart();
            }
        }
        if (key == SDLK_F2 && admin) {
            sound(10);
            client_.sendStart();
        }
        if (key == SDLK_F3) client_.sendTeam(0);
        if (key == SDLK_F4) client_.sendTeam(1);
        // More players at this computer (second key set, gamepads), or fewer.
        if (key == SDLK_F5) client_.sendLocalPlayers(std::min(client_.localPlayers() + 1, net::kMaxLocalPlayers));
        if (key == SDLK_F6) client_.sendLocalPlayers(std::max(client_.localPlayers() - 1, 1));
        if (key == SDLK_ESCAPE) {
            if (!chatText_.empty()) chatText_.clear();
            else if (escape(now)) leave();
        }
        return;
    }
    // In a match or on its result screen.
    if (chatOpen_) {
        if (key == SDLK_BACKSPACE && !chatText_.empty()) chatText_.pop_back();
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            submitChat();
            chatOpen_ = false;
        }
        if (key == SDLK_ESCAPE) {
            chatText_.clear();
            chatOpen_ = false;
        }
        return;
    }
    if (key == SDLK_T) {
        chatOpen_ = true;
        chatText_.clear();
    }
    if (state == State::Result && (key == SDLK_RETURN || key == SDLK_SPACE) && !continueSent_) {
        continueSent_ = true;
        client_.sendContinue();
        sound(10);
    }
    if ((key == SDLK_ESCAPE || (key == SDLK_Q && ctrl)) && escape(now)) leave();
}

void NetUi::update(std::uint64_t nowMs, const std::array<PlayerInput, net::kMaxLocalPlayers>& locals) {
    now_ = nowMs;
    if (mode_ == Mode::Closed) return;
    if (mode_ == Mode::Join) {
        browser_.update(nowMs);
        // The list of servers found may have become shorter under the cursor.
        row_ = std::min(row_, kJoinFields + static_cast<int>(browser_.servers().size()) - 1);
    }
    if (hosting_) server_.update(nowMs);
    if (mode_ != Mode::Connecting && mode_ != Mode::Session) return;

    client_.update(nowMs);
    if (client_.state() == State::Round) {
        for (int l = 0; l < net::kMaxLocalPlayers; ++l) client_.setInput(l, chatOpen_ ? PlayerInput{} : locals[static_cast<std::size_t>(l)]);
        client_.advance(
            nowMs, [this](World& w) { previous_.capture(w); },
            [this](const World& w, const std::vector<Event>& events) {
                if (hooks_.stepped) hooks_.stepped(w, events);
            });
    }
    if (hosting_) server_.update(nowMs);  // what was just sent is handled without a frame's delay

    const State state = client_.state();
    if (state == State::Round && client_.roundId() != lastRound_) {
        lastRound_ = client_.roundId();
        previous_.capture(*client_.world());
        if (hooks_.roundStarted) hooks_.roundStarted(client_.setup().level);
    }
    if (state == lastState_) return;
    if (state == State::Failed) {
        error_ = client_.error();
        const Mode back = entry_;
        close();
        entry_ = back;
        mode_ = Mode::Error;
        return;
    }
    if (state == State::Lobby) {
        mode_ = Mode::Session;
        chatOpen_ = false;
        chatText_.clear();
        lobbyRow_ = 0;
        if (hooks_.lobbyEntered) hooks_.lobbyEntered();
    }
    if (state == State::Round) mode_ = Mode::Session;
    if (state == State::Result) {
        continueSent_ = false;
        if (hooks_.resultShown) hooks_.resultShown(client_.result().winner < 0, client_.result().score.matchWinner >= 0);
    }
    lastState_ = state;
    if (autoStart_ && state == State::Lobby && client_.isAdmin()) {
        autoStart_ = false;
        client_.sendStart();
    }
}

// --- drawing -----------------------------------------------------------------

void NetUi::label(Renderer& r, SpriteBank& bank, const std::string& s, float x, float y, float red, float green, float blue) const {
    r.text(bank, s, x + 1, y + 1, 0, 0, 0);
    r.text(bank, s, x, y, red, green, blue);
}

std::vector<std::string> NetUi::wrap(Renderer& r, SpriteBank& bank, const std::string& s, float width) const {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t end = s.find(' ', i);
        if (end == std::string::npos) end = s.size();
        std::string word = s.substr(i, end - i);
        i = end + 1;
        // A word wider than the line is cut.
        while (r.textWidth(bank, word) > width && word.size() > 1) {
            std::size_t n = word.size() - 1;
            while (n > 1 && r.textWidth(bank, word.substr(0, n)) > width) --n;
            if (!line.empty()) lines.push_back(line), line.clear();
            lines.push_back(word.substr(0, n));
            word.erase(0, n);
        }
        const std::string tried = line.empty() ? word : line + " " + word;
        if (r.textWidth(bank, tried) > width && !line.empty()) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

void NetUi::drawEntry(Renderer& r, SpriteBank& bank, int frame) {
    r.image(bank.picture("glue0"));
    const bool join = mode_ == Mode::Join;
    r.quad(40, 40, 560, 380, 0.0f, 0.0f, 0.10f, 0.85f);
    label(r, bank, join ? "Join Network Game" : "Start Network Game", 55, 50, 1.0f, 0.95f, 0.3f);
    const bool blink = (frame / 20) % 2 == 0;
    auto fieldRow = [&](int row, const std::string& name, const std::string* value, bool secret, const std::string& empty) {
        const float y = 86.0f + 24.0f * static_cast<float>(row);
        const bool on = row == row_;
        std::string shown = secret ? std::string(value->size(), '*') : *value;
        if (editing_ == value) shown += blink ? "_" : " ";
        else if (shown.empty()) shown = empty;
        label(r, bank, name, 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
        label(r, bank, shown, 230, y, editing_ == value ? 0.4f : 1.0f, 1.0f, editing_ == value ? 1.0f : 1.0f);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    };
    if (join) {
        fieldRow(0, "Your name:", &cfg_.netName, false, "Player");
        fieldRow(1, "Address:", &cfg_.netAddress, false, "(host or host:port)");
        fieldRow(2, "Password:", &password_, true, "(none)");
        label(r, bank, "Games on the local network:", 80, 172, 1, 1, 1);
        const auto& servers = browser_.servers();
        if (servers.empty()) label(r, bank, "(searching...)", 100, 196, 0.7f, 0.7f, 0.7f);
        for (std::size_t i = 0; i < servers.size() && i < 8; ++i) {
            static const char* const kPhase[3] = {"in the lobby", "playing", "playing"};
            const net::ServerInfo& info = servers[i].info;
            const float y = 196.0f + 22.0f * static_cast<float>(i);
            const bool on = row_ == kJoinFields + static_cast<int>(i);
            const std::string line = info.name + "   " + std::to_string(info.players) + " player" + (info.players == 1 ? "" : "s") + ", " +
                                     kPhase[static_cast<int>(info.phase)] + (info.password ? "   (password)" : "") +
                                     (info.version != net::kProtocolVersion ? "   (other version)" : "");
            label(r, bank, line, 100, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
            if (on) r.sprite(bank, "cursor1", frame / 8, -1, 82.0f, y + 15.0f);
        }
        label(r, bank, editing_ != nullptr ? "Type, then Enter" : "Up/Down: select   Enter: change / join   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    } else {
        fieldRow(0, "Your name:", &cfg_.netName, false, "Player");
        fieldRow(1, "Game name:", &cfg_.netServerName, false, "");
        fieldRow(2, "Port:", &portText_, false, "");
        fieldRow(3, "Password:", &password_, true, "(none)");
        const float y = 86.0f + 24.0f * 4.0f + 12.0f;
        const bool on = row_ == 4;
        label(r, bank, "Start the server", 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
        const std::vector<std::string> notes = {"Others join with your address and this port (TCP and UDP).",
                                                "On the same network they find the game by themselves.",
                                                "For a server without a window run atomic_server."};
        for (std::size_t i = 0; i < notes.size(); ++i) label(r, bank, notes[i], 60, 250.0f + 22.0f * static_cast<float>(i), 0.75f, 0.75f, 0.75f);
        label(r, bank, editing_ != nullptr ? "Type, then Enter" : "Up/Down: select   Enter: change / start   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    }
}

void NetUi::drawLobby(Renderer& r, SpriteBank& bank, int frame, std::uint64_t nowMs) {
    const net::LobbyState& lobby = client_.lobby();
    const bool waiting = lobby.phase != net::Phase::Lobby;  // joined while a match is on
    const bool admin = client_.isAdmin() && !waiting;
    r.image(bank.picture("glue1"));
    r.quad(14, 10, 300, 306, 0.0f, 0.0f, 0.10f, 0.85f);
    r.quad(322, 10, 304, 306, 0.0f, 0.0f, 0.10f, 0.85f);
    r.quad(14, 322, 612, 132, 0.0f, 0.0f, 0.10f, 0.85f);

    label(r, bank, "Players", 24, 16, 1.0f, 0.95f, 0.3f);
    for (int s = 0; s < kMaxPlayers; ++s) {
        const net::Seat& seat = lobby.seats[static_cast<std::size_t>(s)];
        const float y = 42.0f + 22.0f * static_cast<float>(s);
        const float* c = kSeatColour[s];
        std::string line = std::to_string(s + 1) + (s < 9 ? "   " : "  ");
        if (seat.kind == net::SeatKind::Empty) {
            label(r, bank, line + "-", 24, y, 0.45f, 0.45f, 0.45f);
            continue;
        }
        line += lobby.seatName(s);
        if (seat.kind == net::SeatKind::Human && seat.client == lobby.admin) line += " *";
        label(r, bank, line, 24, y, c[0], c[1], c[2]);
        if (lobby.settings.teamPlay)
            label(r, bank, seat.team == 0 ? "white" : "red", 212, y, seat.team == 0 ? 0.95f : 0.9f, seat.team == 0 ? 0.95f : 0.2f, seat.team == 0 ? 0.95f : 0.2f);
        if (seat.kind == net::SeatKind::Human)
            if (const net::ClientInfo* info = lobby.client(seat.client)) {
                const std::string ping = std::to_string(info->pingMs) + "ms";
                label(r, bank, ping, 304.0f - r.textWidth(bank, ping), y, 0.6f, 0.6f, 0.6f);
            }
    }
    std::string watching;
    for (const net::ClientInfo& c : lobby.clients)
        if (c.seat < 0) watching += (watching.empty() ? "Watching: " : ", ") + c.name;
    if (!watching.empty()) label(r, bank, wrap(r, bank, watching, 280).front(), 24, 268, 0.7f, 0.7f, 0.7f);
    label(r, bank, "* administrator", 24, 290, 0.6f, 0.6f, 0.6f);

    label(r, bank, wrap(r, bank, client_.serverName(), 284).front(), 332, 16, 1.0f, 0.95f, 0.3f);
    for (int row = 0; row < kLobbyRows; ++row) {
        const float y = 40.0f + 18.0f * static_cast<float>(row);
        const bool on = row == lobbyRow_;
        std::string line;
        if (row == 0) {
            const net::ClientInfo* a = lobby.client(lobby.admin);
            line = waiting ? "A match is being played" : admin ? "Start the match" : "Waiting for " + (a != nullptr ? a->name : std::string("the administrator"));
        } else {
            line = wrap(r, bank, optionText(lobby.settings, row - 1), 270).front();
        }
        const float dim = admin || row == 0 ? 1.0f : 0.75f;
        label(r, bank, line, 346, y, on ? 1.0f : 0.85f * dim, on ? 0.95f : 0.85f * dim, on ? 0.3f : 0.85f * dim);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 332.0f, y + 15.0f);
    }

    // Chat: the last lines, then the line being typed.
    std::vector<std::pair<std::string, bool>> lines;
    for (const net::ChatLine& c : client_.chat())
        for (const std::string& part : wrap(r, bank, c.fromServer ? "* " + c.text : c.name + ": " + c.text, 590)) lines.emplace_back(part, c.fromServer);
    const std::size_t shown = std::min<std::size_t>(lines.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [text, fromServer] = lines[lines.size() - shown + i];
        label(r, bank, text, 24, 328.0f + 20.0f * static_cast<float>(i), fromServer ? 0.6f : 1.0f, fromServer ? 0.8f : 1.0f, fromServer ? 1.0f : 1.0f);
    }
    std::string typing = "> " + chatText_;
    while (r.textWidth(bank, typing) > 580 && typing.size() > 3) typing.erase(2, 1);  // the end stays visible
    label(r, bank, typing + ((frame / 20) % 2 == 0 ? "_" : ""), 24, 430, 0.4f, 1.0f, 1.0f);

    const bool leaving = escapeAt_ != 0 && nowMs - escapeAt_ < 3000;
    const std::string hint = leaving ? "Press Esc again to leave the game"
                             : admin ? "Type: chat  Arrows: settings  F2: start  F3/F4: team  F5/F6: players here  Esc"
                                     : "Type: chat   F3/F4: team   F5/F6: players at this computer   Esc: leave";
    label(r, bank, hint, 24, 458, 0.4f, 1.0f, 1.0f);
}

void NetUi::drawChat(Renderer& r, SpriteBank& bank, std::uint64_t nowMs) {
    std::vector<std::pair<std::string, bool>> lines;
    for (const net::ChatLine& c : client_.chat()) {
        if (!chatOpen_ && nowMs - c.atMs > kChatShownMs) continue;
        for (const std::string& part : wrap(r, bank, c.fromServer ? "* " + c.text : c.name + ": " + c.text, 560)) lines.emplace_back(part, c.fromServer);
    }
    const std::size_t shown = std::min<std::size_t>(lines.size(), chatOpen_ ? 6 : 4);
    const float bottom = 428.0f;
    if (chatOpen_) r.quad(20, bottom - 20.0f * static_cast<float>(shown) - 4.0f, 600, 20.0f * static_cast<float>(shown + 1) + 8.0f, 0.0f, 0.0f, 0.10f, 0.75f);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [text, fromServer] = lines[lines.size() - shown + i];
        label(r, bank, text, 28, bottom - 20.0f * static_cast<float>(shown - i), fromServer ? 0.6f : 1.0f, fromServer ? 0.8f : 1.0f, 1.0f);
    }
    if (chatOpen_) {
        std::string typing = "> " + chatText_;
        while (r.textWidth(bank, typing) > 570 && typing.size() > 3) typing.erase(2, 1);
        label(r, bank, typing + "_", 28, bottom, 0.4f, 1.0f, 1.0f);
    }
    if (escapeAt_ != 0 && nowMs - escapeAt_ < 3000) label(r, bank, "Press Esc again to leave the game", 180, 456, 1.0f, 0.95f, 0.3f);
    else if (client_.seat() < 0 && client_.state() == State::Round) label(r, bank, "Watching - you play in the next match   T: chat", 150, 456, 0.8f, 0.8f, 0.8f);
}

void NetUi::drawResult(Renderer& r, SpriteBank& bank) {
    const net::RoundEndMsg& end = client_.result();
    const net::LobbyState& lobby = client_.lobby();
    const World& world = *client_.world();
    auto nameOf = [&](int seat) {
        const std::string name = lobby.seatName(seat);
        return name.empty() || name == "Computer" ? "Player " + std::to_string(seat + 1) : name;
    };
    const bool matchOver = end.score.matchWinner >= 0;
    if (end.campaign != 0) {
        // A campaign stage: the original's notices, on a plain background.
        r.image(bank.picture("glue0"));
        r.quad(100, 170, 440, 130, 0.0f, 0.0f, 0.10f, 0.88f);
        const net::RoundStartMsg& start = client_.roundStart();
        const std::string lines[3] = {end.campaign == 1 ? (end.campaignOver ? "Congratulations!" : "Stage cleared!") : "Oh Well!",
                                      end.campaign == 1 ? (end.campaignOver ? "You made it through the whole campaign!" : "(" + start.stageName + ")")
                                                        : "Campaign unsuccessful!",
                                      "Stage " + std::to_string(start.stage) + " of " + std::to_string(start.stages)};
        for (int i = 0; i < 3; ++i)
            label(r, bank, lines[i], 320.0f - r.textWidth(bank, lines[i]) / 2.0f, 190.0f + 28.0f * static_cast<float>(i), i == 0 ? 1.0f : 0.9f, i == 0 ? 0.95f : 0.9f, i == 0 ? 0.3f : 0.9f);
    } else if (end.winner < 0) {
        r.image(bank.picture("draw"));
    } else if (end.teamPlay) {
        r.image(bank.picture("team" + std::to_string(end.winner)));
        const std::string line = std::string(matchOver ? "TEAM " : "Team ") + std::to_string(end.winner + 1) +
                                 (matchOver ? " WINS THE MATCH!" : " wins the round   score: " + std::to_string(end.score.wins[static_cast<std::size_t>(end.winner)]));
        label(r, bank, line, 210, 440, 1.0f, 0.95f, 0.3f);
    } else if (matchOver) {
        r.image(bank.picture("victory" + std::to_string(end.score.matchWinner)));
        std::string who = nameOf(end.score.matchWinner);
        for (char& ch : who) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const std::string line = who + " WINS THE MATCH!";
        label(r, bank, line, 320.0f - r.textWidth(bank, line) / 2.0f, 440, 1.0f, 0.95f, 0.3f);
    } else {
        r.image(bank.picture("results"));
        label(r, bank, "(Match winner must score " + std::to_string(client_.winsNeeded()) + (client_.setup().winByKills ? " kills)" : " victories)"), 150, 94, 1, 1, 1);
        label(r, bank, "Game Winner was " + nameOf(end.winner) + " !", 150, 140, 1, 1, 1);
        int row = 0;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (!world.player(i).present) continue;
            const std::string line = nameOf(i) + "   score: " + std::to_string(end.score.wins[static_cast<std::size_t>(i)]) + " (kills: " +
                                     std::to_string(end.score.kills[static_cast<std::size_t>(i)]) + ")";
            const bool won = i == end.winner;
            label(r, bank, line, 150, 210.0f + 20.0f * static_cast<float>(row++), won ? 1.0f : 0.8f, won ? 0.95f : 0.8f, won ? 0.3f : 0.8f);
        }
    }
    const std::string hint = continueSent_ ? "Waiting for the other players..." : "Enter: continue   T: chat";
    label(r, bank, hint, 320.0f - r.textWidth(bank, hint) / 2.0f, 460, 0.4f, 1.0f, 1.0f);
}

void NetUi::draw(Renderer& r, SpriteBank& bank, int windowW, int windowH, int frame, std::uint64_t nowMs) {
    const State state = client_.state();
    if (mode_ == Mode::Session && (state == State::Round || state == State::Result) && client_.world() != nullptr) {
        std::array<int, kMaxPlayers> wins = client_.score().wins;
        if (client_.view()->campaign())  // the score boxes show campaign points
            for (int i = 0; i < kMaxPlayers; ++i) wins[static_cast<std::size_t>(i)] = client_.view()->player(i).score;
        r.draw(*client_.view(), previous_, client_.stepAlpha(nowMs), windowW, windowH, &bank, &wins);
        r.begin(windowW, windowH, false);
        if (state == State::Result) drawResult(r, bank);
        drawChat(r, bank, nowMs);
        r.end();
        return;
    }
    r.begin(windowW, windowH);
    if (mode_ == Mode::Join || mode_ == Mode::Host) {
        drawEntry(r, bank, frame);
    } else if (mode_ == Mode::Session) {
        drawLobby(r, bank, frame, nowMs);
    } else {
        r.image(bank.picture("glue0"));
        std::vector<std::string> lines;
        if (mode_ == Mode::Error) {
            lines = wrap(r, bank, error_, 400);
            lines.insert(lines.begin(), "Network game");
        } else {
            lines = {"Network game", hosting_ ? "Starting the server..." : "Connecting to " + connectingTo_ + "..."};
        }
        const float boxH = 70.0f + 24.0f * static_cast<float>(lines.size());
        r.quad(100, 180, 440, boxH, 0.0f, 0.0f, 0.10f, 0.88f);
        for (std::size_t i = 0; i < lines.size(); ++i)
            label(r, bank, lines[i], 320.0f - r.textWidth(bank, lines[i]) / 2.0f, 196.0f + 24.0f * static_cast<float>(i), i == 0 ? 1.0f : 0.9f, i == 0 ? 0.95f : 0.9f, i == 0 ? 0.3f : 0.9f);
        const std::string ok = mode_ == Mode::Error ? "Enter: Ok" : "Esc: cancel";
        label(r, bank, ok, 320.0f - r.textWidth(bank, ok) / 2.0f, 180.0f + boxH - 30.0f, 0.4f, 1.0f, 1.0f);
    }
    r.end();
}

}  // namespace ab
