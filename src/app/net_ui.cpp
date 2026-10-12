#include "app/net_ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <ctime>

#include "app/names.hpp"
#include "rendering/glyphs.hpp"

namespace ab {

namespace {

using State = net::Client::State;

constexpr int kJoinFields = 8;   // name, address, password, join, browse online, join by code, tournament, hide my address; then the servers found, then back
constexpr int kJoinHideRow = 7;
constexpr int kTournamentRows = 5;  // the file, open, play my match, trust the organiser, back
constexpr int kHostRows = 7;     // name, server name, port, password, announce online, start, back
constexpr int kHostOnlineRow = 4;
constexpr int kHostStartRow = 5;
constexpr int kBrowseShown = 12; // games on the list at a time
const char* const kOnlineChoice[3] = {"No", "Listed for everybody", "By its code only"};
constexpr int kLobbySettings = 14;             // the fourteen settings...
constexpr int kLobbyRows = kLobbySettings + 1; // ...and below them the row of actions
constexpr int kMenuItems = 4;
const char* const kMenuItem[kMenuItems] = {"Resume", "Chat", "Back To Lobby", "Leave The Server"};
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
    : cfg_(settings), gameDir_(std::move(gameDir)), userSchemesDir_(std::move(userSchemesDir)), hooks_(std::move(hooks)) {
#ifdef AB_ONLINE
    // The player's lasting key: made on first use and kept beside the settings. Every server
    // joined is shown it; a tournament goes by it.
    if (!userSchemesDir_.empty()) havePlayerKey_ = online::loadOrCreateSeed(userSchemesDir_ + "/../player.key", playerSeed_);
    if (havePlayerKey_) {
        net::Key key{};
        const online::PublicKey pub = online::publicKeyOf(playerSeed_);
        std::copy(pub.begin(), pub.end(), key.begin());
        client_.setPlayerKey(key, [seed = playerSeed_](const std::vector<std::uint8_t>& message) {
            return online::sign(seed, std::string(message.begin(), message.end()));
        });
    }
#endif
}

// A recorded round's record has arrived: held against what this game knows of the round,
// played again, and kept. A record that does not hold is said so in the chat.
void NetUi::takeRecord(std::uint64_t nowMs) {
#ifdef AB_ONLINE
    if (client_.recordSerial() == recordSeen_) return;
    recordSeen_ = client_.recordSerial();
    const std::vector<online::PublicKey> roots = online_.haveConfig() ? online_.config().roots : std::vector<online::PublicKey>{};
    const std::vector<long long> revoked = online_.haveConfig() ? online_.config().revoked : std::vector<long long>{};
    const online::RecordReport report = online::examineRecord(client_.lastRecord(), gameVersion_, roots, revoked, &client_);
    if (!report.holds) {
        ++recordsFailed_;
        client_.note("The record of that round does not hold: " + (report.problems.empty() ? std::string("unreadable") : report.problems.front()), nowMs);
        return;
    }
    ++recordsHeld_;
    if (userSchemesDir_.empty()) return;
    // Kept beside the settings, the newest two hundred.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(userSchemesDir_) / ".." / "records";
    fs::create_directories(dir, ec);
    char stamp[32];
    const std::time_t at = static_cast<std::time_t>(report.record.startedUnix);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::gmtime(&at));
    std::ofstream out(dir / (std::string(stamp) + "-round" + std::to_string(report.record.start.roundId) + ".abrec"), std::ios::binary);
    out << client_.lastRecord();
    std::vector<fs::path> kept;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".abrec") kept.push_back(entry.path());
    std::sort(kept.begin(), kept.end());
    for (std::size_t i = 0; i + 200 < kept.size(); ++i) fs::remove(kept[i], ec);
#else
    (void)nowMs;
#endif
}

std::string NetUi::playerKeyHex() const {
#ifdef AB_ONLINE
    if (havePlayerKey_) return online::toHex(online::publicKeyOf(playerSeed_));
#endif
    return {};
}

// Is the certificate the server showed one that holds? Worked out again when the certificate
// or the configuration it is judged by changes.
void NetUi::judgeCertificate() {
#ifdef AB_ONLINE
    const std::string& shown = client_.certificate();
    const long long serial = online_.haveConfig() ? online_.config().serial : 0;
    if (shown == judgedCertificate_ && serial == judgedSerial_) return;
    judgedCertificate_ = shown;
    judgedSerial_ = serial;
    verifiedOperator_.clear();
    certificateProblem_.clear();
    if (shown.empty()) return;
    if (!online_.haveConfig()) {
        certificateProblem_ = online_.configured() ? "not checked yet" : "cannot be checked (online play is not set up)";
        return;
    }
    online::ServerCertificate cert;
    std::string why;
    if (online::readCertificate(shown, online_.config().roots, online_.config().revoked, static_cast<long long>(std::time(nullptr)), client_.serverIdentityKey(), cert, why))
        verifiedOperator_ = net::cleanText(cert.operatorName, 40);
    else certificateProblem_ = why;
#endif
}

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
    row_ = kHostStartRow;
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
    candidates_.clear();
    expectIdentity_.clear();
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
    menuOpen_ = false;
    osk_.close();
    errorItem_ = 0;
    escapeAt_ = 0;
    lastState_ = State::Idle;
#ifdef AB_ONLINE
    if (hostOnline_) online_.withdraw();  // (sent by the next updates)
#endif
    hostOnline_ = false;
    onlineNote_.clear();
    asking_ = Asking::Nothing;
    viaIce_ = false;
    pathChecked_ = false;
    connectingText_.clear();
#ifdef AB_ONLINE
    iceJoin_.cancel();
    iceHost_.reset();
#endif
    mode_ = Mode::Closed;
}

// Back to the screen the session was started from.
void NetUi::leave() {
    const Mode back = entry_;
    close();
    mode_ = entry_ = back;
    row_ = back == Mode::Host ? kHostStartRow : back == Mode::Join ? 1 : back == Mode::Tournament ? 2 : 0;
    if (back == Mode::Browse) openBrowse();  // with a fresh list
    if (hooks_.lobbyEntered) hooks_.lobbyEntered();
}

bool NetUi::wantsTextInput() const {
    if (editing_ != nullptr) return true;
    return mode_ == Mode::Session && (client_.state() == State::Lobby || chatOpen_);
}

bool NetUi::showingResult() const { return mode_ == Mode::Session && client_.state() == State::Result; }

bool NetUi::inRound() const { return mode_ == Mode::Session && client_.state() == State::Round; }

bool NetUi::padsPlay() const { return inRound() && !chatOpen_ && !menuOpen_ && !osk_.shown(); }

const char* NetUi::screenName() const {
    switch (mode_) {
        case Mode::Closed: return "closed";
        case Mode::Join: return "join";
        case Mode::Host: return "host";
        case Mode::Connecting: return "connecting";
        case Mode::Error: return "error";
        case Mode::Browse: return "browse";
        case Mode::Code: return "code";
        case Mode::Tournament: return "tournament";
        case Mode::Session: break;
    }
    if (client_.roulette() != nullptr) return "wheel";
    return client_.state() == State::Round ? "round" : client_.state() == State::Result ? "result" : "lobby";
}

unsigned NetUi::padKeycode(PadKey k) {
    switch (k) {
        case PadKey::Up: return SDLK_UP;
        case PadKey::Right: return SDLK_RIGHT;
        case PadKey::Down: return SDLK_DOWN;
        case PadKey::Left: return SDLK_LEFT;
        case PadKey::Confirm: return SDLK_RETURN;
        case PadKey::Back: return SDLK_ESCAPE;
        case PadKey::Erase: return SDLK_BACKSPACE;
        case PadKey::Space: return SDLK_SPACE;
        case PadKey::Menu: break;
    }
    return SDLK_APPLICATION;
}

// The time of the last update (the program's clock; a test's own), or the clock itself before the first.
std::uint64_t NetUi::clock() const { return now_ != 0 ? now_ : net::clockMs(); }

void NetUi::edit(std::string* value, std::size_t maxLength, bool digitsOnly, bool pad) {
    editing_ = value;
    editMax_ = maxLength;
    editDigits_ = digitsOnly;
    if (pad) osk_.open(digitsOnly);
}

// The on-screen keyboard is closed: with "done" (a chat line is sent) or without.
void NetUi::finishKeyboard(bool submit) {
    osk_.close();
    if (editing_ != nullptr) {
        // A field keeps what was typed either way, as with Esc on a real keyboard.
        if (mode_ == Mode::Code) row_ = codeText_.size() == 6 ? 1 : 0;
        if (mode_ == Mode::Tournament) {
            editing_ = nullptr;
            if (submit) loadTournament();
            row_ = 1;
            return;
        }
        editing_ = nullptr;
        cfg_.netName = net::cleanText(cfg_.netName, net::kMaxName);
        if (hooks_.settingsChanged) hooks_.settingsChanged();
        return;
    }
    if (submit) submitChat();
    else chatText_.clear();
    chatOpen_ = false;
}

bool NetUi::keyboardKey(unsigned key, bool pad) {
    const int dir = key == SDLK_UP ? 0 : key == SDLK_RIGHT ? 1 : key == SDLK_DOWN ? 2 : key == SDLK_LEFT ? 3 : -1;
    if (dir >= 0) {
        osk_.move(dir);
        if (hooks_.sound) hooks_.sound(20);
        return true;
    }
    // A real keyboard's other keys do what they do without it (Enter and Esc end the typing).
    if (!pad) return false;
    std::string& target = editing_ != nullptr ? *editing_ : chatText_;
    const std::size_t limit = editing_ != nullptr ? editMax_ : static_cast<std::size_t>(net::kMaxChat);
    const bool digits = editing_ != nullptr && editDigits_;
    if (key == SDLK_RETURN) {
        const ScreenKeyboard::Press press = osk_.confirm();
        if (press.action == ScreenKeyboard::Action::Text) appendTyped(target, press.text.c_str(), limit, digits);
        if (press.action == ScreenKeyboard::Action::Erase) removeLastChar(target);
        if (press.action == ScreenKeyboard::Action::Done) finishKeyboard(true);
        if (hooks_.sound) hooks_.sound(10);
    }
    if (key == SDLK_BACKSPACE) removeLastChar(target);
    if (key == SDLK_SPACE && !digits) appendTyped(target, " ", limit, false);
    if (key == SDLK_APPLICATION) finishKeyboard(true);
    if (key == SDLK_ESCAPE) finishKeyboard(false);
    return true;
}

// The row of actions under the lobby's settings.
std::vector<NetUi::LobbyItem> NetUi::lobbyItems() const {
    const net::LobbyState& lobby = client_.lobby();
    const bool open = lobby.phase == net::Phase::Lobby;  // no match is on
    std::vector<LobbyItem> items;
    if (client_.isAdmin()) items.push_back({LobbyAction::Start, 0, "Start", open});
    else items.push_back({LobbyAction::Ready, 0, std::string(client_.ready() ? "[x]" : "[ ]") + " Ready", open && client_.seat() >= 0});
    if (lobby.settings.teamPlay) {
        const std::array<int, net::kMaxLocalPlayers> seats = client_.seats();
        int seated = 0;
        for (int seat : seats) seated += seat >= 0 ? 1 : 0;
        for (int l = 0; l < net::kMaxLocalPlayers; ++l)
            if (seats[static_cast<std::size_t>(l)] >= 0) items.push_back({LobbyAction::Team, l, seated > 1 ? "Team " + std::to_string(l + 1) : "Team", open});
    }
    items.push_back({LobbyAction::Players, 0, "Players: " + std::to_string(client_.localPlayers()), open});
    items.push_back({LobbyAction::Chat, 0, "Chat", true});
    items.push_back({LobbyAction::Leave, 0, "Leave", true});
    return items;
}

void NetUi::lobbyAct(const LobbyItem& item, bool pad) {
    if (hooks_.sound) hooks_.sound(item.enabled ? 10 : 40);
    if (!item.enabled) return;
    switch (item.action) {
        case LobbyAction::Start: client_.sendStart(); break;  // refused by the server, with the reason in the chat, if somebody is not ready
        case LobbyAction::Ready: client_.sendReady(!client_.ready()); break;
        case LobbyAction::Team: client_.sendTeam(item.local); break;
        // One more player at this computer; after the fourth, one again.
        case LobbyAction::Players: client_.sendLocalPlayers(client_.localPlayers() % net::kMaxLocalPlayers + 1); break;
        case LobbyAction::Chat:
            if (pad) osk_.open(false);  // a keyboard types into the chat line as it is
            break;
        case LobbyAction::Leave: leave(); break;
    }
}

void NetUi::connectTo(const std::string& address, bool remember) {
    if (remember) {
        cfg_.netAddress = address;
        if (hooks_.settingsChanged) hooks_.settingsChanged();
    }
    connectingTo_ = address;
    connectingText_.clear();
    viaIce_ = false;
    connectingSince_ = clock();
    client_.expectIdentity(expectIdentity_);  // set for a game the master server named; empty otherwise
#ifdef AB_ONLINE
    // The keys that may vouch for a server are in the configuration: fetched now, if this
    // copy of the game knows where, so that a certificate can be checked whichever way the server was found.
    if (!online_.configured() && !localOnly_) {
        online::Bootstrap bootstrap;
        std::string why;
        const bool given = !onlineUrls_.empty() || !onlineKey_.empty();
        if (online::chooseBootstrap(given ? onlineUrls_ : cfg_.onlineConfigUrl, given ? onlineKey_ : cfg_.onlineConfigKey, bootstrap, why)) {
            online_.configure(bootstrap, userSchemesDir_.empty() ? std::string() : userSchemesDir_ + "/../online-serial.txt", playerName(), net::kProtocolVersion);
            onlineName_ = playerName();
        }
    }
    online_.loadConfig();
#endif
    client_.setPrediction(cfg_.netPrediction);
    if (!userSchemesDir_.empty()) client_.setKnownServersFile(userSchemesDir_ + "/../known_servers.txt");
    client_.connect(address, playerName(), password_, clock());
    lastState_ = State::Connecting;
    lastRound_ = 0;
    mode_ = Mode::Connecting;
}

bool NetUi::onlineBusy() const {
#ifdef AB_ONLINE
    return online_.busy();
#else
    return false;
#endif
}

void NetUi::showError(const std::string& why) {
    error_ = why;
    identityChanged_ = false;
    errorItem_ = 0;
    asking_ = Asking::Nothing;
    mode_ = Mode::Error;
}

// The session with the master server is set up on first use (and again under a new name).
bool NetUi::onlineBegin() {
#ifdef AB_ONLINE
    if (online_.configured() && onlineName_ == playerName()) return true;
    online::Bootstrap bootstrap;
    std::string why;
    const bool given = !onlineUrls_.empty() || !onlineKey_.empty();
    if (!online::chooseBootstrap(given ? onlineUrls_ : cfg_.onlineConfigUrl, given ? onlineKey_ : cfg_.onlineConfigKey, bootstrap, why)) {
        showError(why);
        return false;
    }
    online_.configure(bootstrap, userSchemesDir_.empty() ? std::string() : userSchemesDir_ + "/../online-serial.txt", playerName(), net::kProtocolVersion);
    onlineName_ = playerName();
    return true;
#else
    showError("This copy of the game was built without online play");
    return false;
#endif
}

void NetUi::openBrowse() {
    mode_ = Mode::Join;
    entry_ = Mode::Join;
    if (!onlineBegin()) return;
    mode_ = entry_ = Mode::Browse;
    row_ = 0;
#ifdef AB_ONLINE
    online_.refreshLobbies();
#endif
}

void NetUi::openCode(bool pad) {
    mode_ = Mode::Join;
    entry_ = Mode::Join;
    if (!onlineBegin()) return;
    mode_ = entry_ = Mode::Code;
    row_ = 0;
    codeText_.clear();
    edit(&codeText_, 6, true, pad);  // straight to typing it
}

// --- tournaments ---

void NetUi::openTournament() {
    mode_ = entry_ = Mode::Tournament;
    row_ = cfg_.tournament.empty() ? 0 : 1;
    if (!cfg_.tournament.empty() && !tournamentLoaded_ && !tournamentLoading_) loadTournament();
}

// Fetches the file named on the screen (a path is read at once; an address on a thread of its own).
void NetUi::loadTournament() {
#ifdef AB_ONLINE
    std::string location, keyHex;
    online::splitInvitation(cfg_.tournament, location, keyHex);
    // An invitation "location#key" says whose signature to expect; another file than last time starts without a key.
    if (!keyHex.empty() || location != tournamentOpened_) {
        if (location != tournamentOpened_ && keyHex.empty() && !tournamentOpened_.empty()) cfg_.tournamentKey.clear(), cfg_.tournamentSerial = 0;
        if (!keyHex.empty() && keyHex != cfg_.tournamentKey) cfg_.tournamentKey = keyHex, cfg_.tournamentSerial = 0;
    }
    cfg_.tournament = location;
    tournamentOpened_ = location;
    if (hooks_.settingsChanged) hooks_.settingsChanged();
    tournamentLoaded_ = false;
    tournamentNote_ = "Reading...";
    if (location.empty()) {
        tournamentNote_ = "Name a tournament file or its address";
        return;
    }
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        online::HttpRequest q;
        q.url = location;
        tournamentJob_.start(q);
        tournamentLoading_ = true;
        return;
    }
    std::string envelope, why;
    if (!online::fetchTournament(location, envelope, why)) tournamentNote_ = why;
    else readTournamentFile(envelope);
#else
    tournamentNote_ = "This copy of the game was built without online play";
#endif
}

void NetUi::readTournamentFile(const std::string& envelope) {
#ifdef AB_ONLINE
    online::PublicKey trusted{};
    const bool haveKey = online::fromHex(cfg_.tournamentKey, trusted.data(), trusted.size());
    online::Tournament t;
    std::string why;
    bool otherKey = false;
    if (!online::readTournament(envelope, haveKey ? &trusted : nullptr, haveKey ? cfg_.tournamentSerial : 0, t, why)) {
        // Signed, but not with the key chosen before (another tournament, or the organiser has
        // a new key): shown as any file whose signer the player has not yet chosen to believe.
        otherKey = haveKey && why == "not signed by the organiser whose key you hold" && online::readTournament(envelope, nullptr, 0, t, why);
        if (!otherKey) {
            tournamentLoaded_ = false;
            tournamentNote_ = "Not opened: " + why;
            return;
        }
    }
    tournament_ = std::move(t);
    tournamentLoaded_ = true;
    tournamentTrusted_ = haveKey && !otherKey;
    if (haveKey && tournament_.serial != cfg_.tournamentSerial) {
        cfg_.tournamentSerial = tournament_.serial;
        if (hooks_.settingsChanged) hooks_.settingsChanged();
    }
    // What it means for this player.
    online::PublicKey mine{};
    if (havePlayerKey_) mine = online::publicKeyOf(playerSeed_);
    const online::TournamentPlayer* me = havePlayerKey_ ? tournament_.playerByKey(mine) : nullptr;
    const std::vector<const online::TournamentMatch*> open = tournament_.openMatchesOf(mine);
    if (!tournamentTrusted_)
        tournamentNote_ = std::string(otherKey ? "Not the key you chose before. " : "") + "Signed by " + online::keyFingerprint(tournament_.organiserKey) + ". Is that your organiser's key?";
    else if (me == nullptr) tournamentNote_ = "You are not among its players";
    else if (open.empty()) tournamentNote_ = me->name + ": no match left to play";
    else if (!tournament_.ready(*open.front())) tournamentNote_ = me->name + ": match " + open.front()->id + " waits for an earlier match";
    else {
        std::string others;
        for (const std::string& who : open.front()->players)
            if (who != me->name) others += (others.empty() ? "" : ", ") + who;
        tournamentNote_ = me->name + ": match " + open.front()->id + " against " + others;
    }
#else
    (void)envelope;
#endif
}

// To the server the file names for this player's next match; it must show the identity the file names.
void NetUi::playTournamentMatch() {
#ifdef AB_ONLINE
    if (!tournamentLoaded_ || !tournamentTrusted_ || !havePlayerKey_) return;
    const std::vector<const online::TournamentMatch*> open = tournament_.openMatchesOf(online::publicKeyOf(playerSeed_));
    if (open.empty() || !tournament_.ready(*open.front())) return;
    const online::TournamentServer* server = tournament_.server(open.front()->server);
    candidates_ = {server->address};
    candidate_ = 0;
    expectIdentity_ = server->identityKey;
    password_.clear();
    connectTo(server->address, false);
#endif
}

// The master server is asked how to reach that game; update() goes on from its answer.
void NetUi::askJoin(const std::string& lobbyId, const std::string& code) {
#ifdef AB_ONLINE
    connectingTo_ = "the master server";
    connectingSince_ = clock();
    lastState_ = State::Idle;
    mode_ = Mode::Connecting;
    if (lobbyId.empty()) {
        asking_ = Asking::Find;
        online_.findByCode(code);
    } else {
        asking_ = Asking::Join;
        online_.join(lobbyId, code);
    }
#else
    (void)lobbyId;
    (void)code;
#endif
}

void NetUi::updateOnline(std::uint64_t nowMs) {
#ifdef AB_ONLINE
    if (hosting_ && hostOnline_) {
        announcer_.update(nowMs, online_, server_, net::cleanText(cfg_.netServerName, 32), cfg_.netOnline == 1, true);
        iceHost_.update(nowMs, online_, server_, cfg_.netHideAddress);  // players behind routers find a way in
        if (online_.announced()) onlineNote_ = std::string(cfg_.netOnline == 1 ? "Listed, code " : "Code ") + online_.code();
        else onlineNote_ = online_.error().empty() ? "Announcing..." : "Not announced online";
    }
    if (tournamentLoading_ && tournamentJob_.finished()) {
        tournamentLoading_ = false;
        const online::HttpResponse r = tournamentJob_.take();
        if (r.ok()) readTournamentFile(r.body);
        else tournamentNote_ = "Not fetched: " + (r.status == 0 ? r.error : "the server answered " + std::to_string(r.status));
    }
    if (asking_ == Asking::Way) {
        // A way to the host through the routers is being looked for.
        iceJoin_.update(nowMs, online_);
        connectingText_ = iceJoin_.step();
        if (iceJoin_.state() == online::IceJoin::State::Ready) {
            asking_ = Asking::Nothing;
            connectOver(iceJoin_.take());
        } else if (iceJoin_.state() == online::IceJoin::State::Failed) {
            // None: the host's address as the master saw it, if it gave one.
            asking_ = Asking::Nothing;
            iceJoin_.cancel();
            viaIce_ = true;  // (so that the first address is the next thing tried)
            if (!nextCandidate()) showError("No way to that game was found (its host cannot be reached from here)");
        }
        return;
    }
    if (asking_ == Asking::Nothing || online_.busy()) return;
    if (!online_.error().empty()) return showError(online_.error());
    if (asking_ == Asking::Find) {
        online::LobbyInfo found;
        if (!online_.takeFound(found)) return showError("No game with that code");
        asking_ = Asking::Join;
        online_.join(found.id, codeText_);
        return;
    }
    online::JoinInfo info;
    if (!online_.takeJoin(info)) return showError("The master server gave no answer");
    asking_ = Asking::Nothing;
    if (info.candidates.empty()) return showError("That game has not said how it can be reached");
    // The ways in are tried one after the other; whoever answers must show the identity the master named.
    candidates_.clear();
    bool offersIce = false;
    for (const std::string& c : info.candidates) {
        if (c == online::kIceCandidate) offersIce = true;
        else candidates_.push_back(c);
    }
    candidate_ = 0;
    expectIdentity_ = info.identityKey;
    if (offersIce) {
        // First a way through the routers (straight, or by the relay); the host's address after that.
        iceJoin_.start(online_, info, cfg_.netHideAddress, nowMs);
        asking_ = Asking::Way;
        connectingText_ = iceJoin_.step();
        return;
    }
    if (candidates_.empty()) return showError("That game has not said how it can be reached");
    connectTo(candidates_[0], false);
#else
    (void)nowMs;
#endif
}

// The same over a way through routers that ICE has found.
void NetUi::connectOver(std::unique_ptr<net::Carrier> way) {
    connectingTo_ = "the host";
    connectingText_.clear();
    connectingSince_ = clock();
    viaIce_ = true;
    pathChecked_ = false;
    client_.expectIdentity(expectIdentity_);
    client_.setPrediction(cfg_.netPrediction);
    client_.connectVia(std::move(way), "the host", playerName(), password_, clock());
    lastState_ = State::Connecting;
    lastRound_ = 0;
    mode_ = Mode::Connecting;
}

bool NetUi::nextCandidate() {
    const std::size_t next = viaIce_ ? 0 : candidate_ + 1;
    if (next >= candidates_.size()) return false;
    viaIce_ = false;
    candidate_ = next;
    connectTo(candidates_[candidate_], false);
    return true;
}

void NetUi::startHost() {
    const int port = std::atoi(portText_.c_str());
    if (port < 1 || port > 65535) {
        error_ = "The port must be a number from 1 to 65535";
        mode_ = Mode::Error;
        return;
    }
    candidates_.clear();
    expectIdentity_.clear();
    hostOnline_ = false;
    if (cfg_.netOnline != 0) {
        // A game the master announces has no password: the master would hand out the way to it
        // before the password is asked for. A private game is one found by its code only.
        if (!password_.empty()) return showError("A game announced online has no password. To keep it private, announce it \"by its code only\".");
        if (!onlineBegin()) return;
        hostOnline_ = true;
        onlineNote_ = "Announcing...";
#ifdef AB_ONLINE
        announcer_.reset();
#endif
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
    config.upnp = !localOnly_;
    config.discoverable = !localOnly_;
#ifdef AB_ONLINE
    config.verifySignature = [](const net::Key& key, const std::vector<std::uint8_t>& message, const std::array<std::uint8_t, 64>& signature) {
        online::PublicKey k{};
        std::copy(key.begin(), key.end(), k.begin());
        return online::verify(k, std::string(message.begin(), message.end()), signature);
    };
#endif
    if (!userSchemesDir_.empty()) config.banFile = userSchemesDir_ + "/../bans.txt";  // beside the settings file
    if (!userSchemesDir_.empty()) config.identityFile = userSchemesDir_ + "/../server.key";  // a hosted game tries to open the router's port; the lobby says how it went
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
    // The administrator's commands: "/kick NAME" (back in five minutes at the earliest),
    // "/ban NAME" (the address stays out), "/unban ADDRESS".
    if (text.rfind("/kick ", 0) == 0 || text.rfind("/ban ", 0) == 0) {
        const bool ban = text[1] == 'b';
        const std::string who = text.substr(ban ? 5 : 6);
        for (const net::ClientInfo& c : client_.lobby().clients)
            if (c.name == who && c.id != client_.id()) ban ? client_.sendBan(c.id) : client_.sendKick(c.id);
        return;
    }
    // "/admin NAME": the administrator hands the role over. "/login PASSWORD": the server
    // owner takes it, with the server's administrator password.
    if (text.rfind("/admin ", 0) == 0) {
        for (const net::ClientInfo& c : client_.lobby().clients)
            if (c.name == text.substr(7) && c.id != client_.id()) client_.sendAdmin(c.id);
        return;
    }
    if (text.rfind("/login ", 0) == 0) {
        client_.sendLogin(text.substr(7));
        return;
    }
    if (text.rfind("/unban ", 0) == 0) {
        client_.sendUnban(text.substr(7));
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
    // Whatever the font can show: ASCII and the accented letters it has (n with tilde,
    // the acute vowels, the inverted marks...).
    appendTyped(*target, utf8, limit, digits);
}

void NetUi::key(unsigned key, bool ctrl, bool pad) {
    auto sound = [&](int id) {
        if (hooks_.sound) hooks_.sound(id);
    };
    const std::uint64_t now = clock();
    if (key != SDLK_ESCAPE) escapeAt_ = 0;
    if (osk_.shown() && keyboardKey(key, pad)) return;
    if (osk_.shown() && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE)) {
        // Enter or Esc on a real keyboard end the typing, and the on-screen keyboard with it.
        osk_.close();
        if (editing_ == nullptr && chatText_.empty()) {
            chatOpen_ = false;
            return;
        }
    }

    if (mode_ == Mode::Error) {
        // A server whose identity changed: the second item (or F8) forgets the old one, so
        // that joining again accepts the new (for when the server really was set up anew).
        if (identityChanged_ && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN)) errorItem_ ^= 1, sound(20);
        if (identityChanged_ && (key == SDLK_F8 || (errorItem_ == 1 && (key == SDLK_RETURN || key == SDLK_KP_ENTER)))) {
            client_.forgetServer(connectingTo_);
            identityChanged_ = false;
            errorItem_ = 0;
            error_ = "The old identity is forgotten. Join again to accept the new one.";
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_ESCAPE) {
            errorItem_ = 0;
            mode_ = entry_;
            row_ = entry_ == Mode::Host ? kHostStartRow : entry_ == Mode::Join ? 1 : entry_ == Mode::Tournament ? 2 : 0;
            if (entry_ == Mode::Browse) openBrowse();
        }
        return;
    }
    if (mode_ == Mode::Connecting) {
        // "Cancel" is the one item. Enter counts only after a moment, so that the Enter
        // that started the connection, pressed twice, does not end it.
        if (key == SDLK_ESCAPE || ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && now - connectingSince_ > 700)) leave();
        return;
    }
    if (mode_ == Mode::Join || mode_ == Mode::Host) {
        if (editing_ != nullptr) {
            if (key == SDLK_BACKSPACE) removeLastChar(*editing_);
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                const bool wasAddress = editing_ == &cfg_.netAddress;
                editing_ = nullptr;
                osk_.close();
                cfg_.netName = net::cleanText(cfg_.netName, net::kMaxName);
                if (hooks_.settingsChanged) hooks_.settingsChanged();
                // Enter on the address joins at once.
                if (wasAddress && key != SDLK_ESCAPE && !net::cleanText(cfg_.netAddress, 64).empty()) {
                    sound(10);
                    candidates_.clear();
                    expectIdentity_.clear();
                    connectTo(net::cleanText(cfg_.netAddress, 64), true);
                }
            }
            return;
        }
        const int servers = mode_ == Mode::Join ? static_cast<int>(browser_.servers().size()) : 0;
        const int rows = mode_ == Mode::Join ? kJoinFields + servers + 1 : kHostRows;
        if (key == SDLK_UP) row_ = (row_ + rows - 1) % rows, sound(20);
        if (key == SDLK_DOWN) row_ = (row_ + 1) % rows, sound(20);
        if ((key == SDLK_LEFT || key == SDLK_RIGHT) && mode_ == Mode::Host && row_ == kHostOnlineRow) {
            cfg_.netOnline = (cfg_.netOnline + (key == SDLK_LEFT ? 2 : 1)) % 3;
            sound(20);
            if (hooks_.settingsChanged) hooks_.settingsChanged();
        }
        if (key == SDLK_ESCAPE) close();
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            sound(10);
            if (mode_ == Mode::Join) {
                const std::string address = net::cleanText(cfg_.netAddress, 64);
                if (row_ == 0) edit(&cfg_.netName, net::kMaxName, false, pad);
                else if (row_ == 1) edit(&cfg_.netAddress, 64, false, pad);
                else if (row_ == 2) edit(&password_, 32, false, pad);
                else if (row_ == 3 && address.empty()) sound(40);
                else if (row_ == 4) openBrowse();
                else if (row_ == 5) openCode(pad);
                else if (row_ == 6) openTournament();
                else if (row_ == kJoinHideRow) {
                    cfg_.netHideAddress = !cfg_.netHideAddress;
                    if (hooks_.settingsChanged) hooks_.settingsChanged();
                }
                else if (row_ >= kJoinFields + servers) close();
                else {
                    candidates_.clear();
                    expectIdentity_.clear();
                    if (row_ == 3) connectTo(address, true);
                    else connectTo(browser_.servers()[static_cast<std::size_t>(row_ - kJoinFields)].address.text(), false);
                }
            } else {
                if (row_ == 0) edit(&cfg_.netName, net::kMaxName, false, pad);
                else if (row_ == 1) edit(&cfg_.netServerName, 32, false, pad);
                else if (row_ == 2) edit(&portText_, 5, true, pad);
                else if (row_ == 3) edit(&password_, 32, false, pad);
                else if (row_ == kHostOnlineRow) {
                    cfg_.netOnline = (cfg_.netOnline + 1) % 3;
                    if (hooks_.settingsChanged) hooks_.settingsChanged();
                } else if (row_ == kHostStartRow) startHost();
                else close();
            }
        }
        return;
    }
#ifdef AB_ONLINE
    if (mode_ == Mode::Browse) {
        // The games the master lists, then Refresh and Back.
        const int games = static_cast<int>(online_.lobbies().size());
        const int rows = games + 2;
        row_ = std::clamp(row_, 0, rows - 1);
        if (key == SDLK_UP) row_ = (row_ + rows - 1) % rows, sound(20);
        if (key == SDLK_DOWN) row_ = (row_ + 1) % rows, sound(20);
        if (key == SDLK_ESCAPE) openJoin();
        if (key == SDLK_F5 || ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && row_ == games)) {
            sound(10);
            if (!online_.busy()) online_.refreshLobbies();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            sound(10);
            if (row_ == games + 1) openJoin();
            else if (online_.lobbies()[static_cast<std::size_t>(row_)].version != net::kProtocolVersion) sound(40);
            else askJoin(online_.lobbies()[static_cast<std::size_t>(row_)].id, "");
        }
        return;
    }
    if (mode_ == Mode::Tournament) {
        if (editing_ != nullptr) {
            if (key == SDLK_BACKSPACE) removeLastChar(*editing_);
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                editing_ = nullptr;
                osk_.close();
                if (key != SDLK_ESCAPE) loadTournament();
                row_ = 1;
            }
            return;
        }
        const bool canPlay = tournamentLoaded_ && tournamentTrusted_ && tournamentNote_.find(": match ") != std::string::npos && tournamentNote_.find("waits") == std::string::npos;
        if (key == SDLK_UP) row_ = (row_ + kTournamentRows - 1) % kTournamentRows, sound(20);
        if (key == SDLK_DOWN) row_ = (row_ + 1) % kTournamentRows, sound(20);
        if (key == SDLK_ESCAPE) openJoin();
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (row_ == 0) {
                sound(10);
                edit(&cfg_.tournament, 300, false, pad);
            } else if (row_ == 1) {
                sound(10);
                if (!tournamentLoading_) loadTournament();
            } else if (row_ == 2) {
                sound(canPlay ? 10 : 40);
                if (canPlay) playTournamentMatch();
            } else if (row_ == 3) {
                // The player's own choice: this is the organiser whose word counts for this tournament.
                const bool ask = tournamentLoaded_ && !tournamentTrusted_;
                sound(ask ? 10 : 40);
                if (ask) {
                    cfg_.tournamentKey = online::toHex(tournament_.organiserKey);
                    cfg_.tournamentSerial = tournament_.serial;
                    if (hooks_.settingsChanged) hooks_.settingsChanged();
                    loadTournament();
                }
            } else {
                sound(10);
                openJoin();
            }
        }
        return;
    }
    if (mode_ == Mode::Code) {
        // The code (six digits), Join, Back.
        if (editing_ != nullptr) {
            if (key == SDLK_BACKSPACE) removeLastChar(*editing_);
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                editing_ = nullptr;
                osk_.close();
                row_ = codeText_.size() == 6 ? 1 : 0;
            }
            return;
        }
        if (key == SDLK_UP) row_ = (row_ + 2) % 3, sound(20);
        if (key == SDLK_DOWN) row_ = (row_ + 1) % 3, sound(20);
        if (key == SDLK_ESCAPE) openJoin();
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (row_ == 0) edit(&codeText_, 6, true, pad);
            else if (row_ == 2) openJoin();
            else if (codeText_.size() != 6) sound(40);
            else askJoin("", codeText_);
            if (row_ != 1 || codeText_.size() == 6) sound(10);
        }
        return;
    }
#endif
    if (mode_ != Mode::Session) return;

    const State state = client_.state();
    if (state == State::Lobby) {
        const bool admin = client_.isAdmin() && client_.lobby().phase == net::Phase::Lobby;
        const std::vector<LobbyItem> items = lobbyItems();
        const int count = static_cast<int>(items.size());
        lobbyAction_ = std::clamp(lobbyAction_, 0, count - 1);
        const bool onActions = lobbyRow_ == kLobbySettings;
        if (key == SDLK_BACKSPACE) removeLastChar(chatText_);
        if (key == SDLK_UP) lobbyRow_ = (lobbyRow_ + kLobbyRows - 1) % kLobbyRows, sound(20);
        if (key == SDLK_DOWN) lobbyRow_ = (lobbyRow_ + 1) % kLobbyRows, sound(20);
        if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            if (onActions) {
                lobbyAction_ = (lobbyAction_ + (key == SDLK_LEFT ? count - 1 : 1)) % count;
                sound(20);
            } else {
                if (admin) client_.sendOption(static_cast<net::Option>(lobbyRow_), key == SDLK_LEFT ? -1 : 1);
                sound(admin ? 20 : 40);
            }
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (!chatText_.empty()) {
                submitChat();
            } else if (onActions) {
                lobbyAct(items[static_cast<std::size_t>(lobbyAction_)], pad);
            } else {
                // On a setting: the next value.
                if (admin) client_.sendOption(static_cast<net::Option>(lobbyRow_), 1);
                sound(admin ? 20 : 40);
            }
        }
        // Start on a gamepad: to the actions.
        if (key == SDLK_APPLICATION) lobbyRow_ = kLobbySettings, lobbyAction_ = 0, sound(20);
        // The keys of before, as shortcuts.
        if (key == SDLK_F2 && admin) {
            sound(10);
            client_.sendStart();
        }
        if (key == SDLK_F7 && !client_.isAdmin() && client_.lobby().phase == net::Phase::Lobby) {
            sound(10);
            client_.sendReady(!client_.ready());
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
    // In a match or on its result screen: the menu (Start on a gamepad) first.
    if (menuOpen_) {
        if (key == SDLK_UP) menuItem_ = (menuItem_ + kMenuItems - 1) % kMenuItems, sound(20);
        if (key == SDLK_DOWN) menuItem_ = (menuItem_ + 1) % kMenuItems, sound(20);
        if (key == SDLK_ESCAPE || key == SDLK_APPLICATION) menuOpen_ = false;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            sound(10);
            menuOpen_ = false;
            if (menuItem_ == 1) {
                chatOpen_ = true;
                chatText_.clear();
                if (pad) osk_.open(false);
            }
            if (menuItem_ == 2) client_.leaveMatch();
            if (menuItem_ == 3) leave();
        }
        return;
    }
    // In a match or on its result screen.
    if (chatOpen_) {
        if (key == SDLK_BACKSPACE) removeLastChar(chatText_);
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
    if (key == SDLK_APPLICATION) {
        menuOpen_ = true;
        menuItem_ = 0;
        escapeAt_ = 0;
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
    // Out of the match, not off the server: back to the lobby. (Esc twice there leaves the server.)
    if ((key == SDLK_ESCAPE || (key == SDLK_Q && ctrl)) && escape(now)) {
        escapeAt_ = 0;
        client_.leaveMatch();
    }
}

void NetUi::update(std::uint64_t nowMs, const std::array<PlayerInput, net::kMaxLocalPlayers>& locals) {
    now_ = nowMs;
#ifdef AB_ONLINE
    online_.update();  // also when closed: a game withdrawn is still to be told to the master
#endif
    if (mode_ == Mode::Closed) return;
    if (mode_ == Mode::Join) {
        browser_.update(nowMs);
        // The list of servers found may have become shorter under the cursor.
        // "Back", after them, stays under a cursor that was on it.
        const int servers = static_cast<int>(browser_.servers().size());
        row_ = row_ == kJoinFields + shownServers_ ? kJoinFields + servers : std::min(row_, kJoinFields + servers);
        shownServers_ = servers;
    }
    if (hosting_) server_.update(nowMs);
    updateOnline(nowMs);
    if (mode_ != Mode::Connecting && mode_ != Mode::Session) return;
    judgeCertificate();
    takeRecord(nowMs);

    client_.update(nowMs);
    if (client_.state() == State::Round) {
        for (int l = 0; l < net::kMaxLocalPlayers; ++l) client_.setInput(l, padsPlay() ? locals[static_cast<std::size_t>(l)] : PlayerInput{});
        client_.advance(
            nowMs, [this](World& w) { previous_.capture(w); },
            [this](const World& w, const std::vector<Event>& events) {
                if (hooks_.stepped) hooks_.stepped(w, events);
            });
    }
    if (hosting_) server_.update(nowMs);  // what was just sent is handled without a frame's delay

    // The bonus wheel: the same wheel as the server's, run from its seed at 25 frames a second.
    if (const net::RouletteMsg* show = client_.roulette()) {
        if (!wheel_ || wheelSeed_ != show->seed) {
            wheel_ = std::make_unique<Roulette>(wheelValues_, show->seed);
            wheelSeed_ = show->seed;
            wheelFrames_ = 0;
        }
        const int due = static_cast<int>((nowMs - client_.rouletteSince()) / 40);
        for (int n = 0; wheelFrames_ < due && n < 200; ++n, ++wheelFrames_) {
            const bool wasStopped = wheel_->state() == Roulette::State::Stopped;
            const int ticks = wheel_->step();
            if (ticks > 0 && hooks_.sound) hooks_.sound(1300);
            if (!wasStopped && wheel_->state() == Roulette::State::Stopped && hooks_.sound) hooks_.sound(show->prize == kPowClog ? 1320 : 1310);
            if (wheelFrames_ == 50 || wheel_->state() == Roulette::State::Stopped) wheel_->press();
        }
    } else {
        wheel_.reset();
    }

    const State state = client_.state();
    if (state == State::Round && client_.roundId() != lastRound_) {
        lastRound_ = client_.roundId();
        previous_.capture(*client_.world());
        if (hooks_.roundStarted) hooks_.roundStarted(client_.setup().level);
    }
    if (state == lastState_) return;
    // A way through routers is tried out before the lobby is shown: 64 KB each way. A way
    // that stops carrying is given up for the next one.
    if (state == State::Lobby && viaIce_ && !pathChecked_) {
        if (client_.volumeCheck() == net::Client::VolumeCheck::NotRun) client_.startVolumeCheck(nowMs);
        connectingText_ = "Testing the connection...";
        if (client_.volumeCheck() == net::Client::VolumeCheck::Running) return;
        if (client_.volumeCheck() == net::Client::VolumeCheck::Failed) {
            client_.disconnect();
#ifdef AB_ONLINE
            // Once more by the relay over TCP, which such a network lets through.
            if (iceJoin_.again(online_, nowMs)) {
                asking_ = Asking::Way;
                mode_ = Mode::Connecting;
                connectingText_ = iceJoin_.step();
                lastState_ = client_.state();
                return;
            }
#endif
            if (!nextCandidate()) showError("The way found to that game stopped carrying data, and there is no other");
            return;
        }
        pathChecked_ = true;
        connectingText_.clear();
    }
    if (state == State::Failed) {
        // Nobody at that address (or on that way): the next one the master named, if there is one.
        if (!client_.answered() && nextCandidate()) return;
        error_ = client_.error();
        identityChanged_ = client_.identityChanged();
        const Mode back = entry_;
        close();
        entry_ = back;
        mode_ = Mode::Error;
        return;
    }
    if (state == State::Lobby) {
        mode_ = Mode::Session;
        chatOpen_ = false;
        menuOpen_ = false;
        osk_.close();
        chatText_.clear();
        lobbyRow_ = kLobbySettings;
        lobbyAction_ = 0;
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
    // A row that does something when chosen.
    auto item = [&](int row, const std::string& text, float y) {
        const bool on = row == row_;
        label(r, bank, text, 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    };
    if (join) {
        fieldRow(0, "Your name:", &cfg_.netName, false, "Player");
        fieldRow(1, "Address:", &cfg_.netAddress, false, "(host or host:port)");
        fieldRow(2, "Password:", &password_, true, "(none)");
        item(3, "Join this address", 86.0f + 24.0f * 3.0f);
        item(4, "Browse online games", 86.0f + 24.0f * 4.0f);
        item(5, "Join online by code", 86.0f + 24.0f * 5.0f);
        item(6, "Tournament", 86.0f + 24.0f * 6.0f);
        item(kJoinHideRow, std::string("Online, hide my address: ") + (cfg_.netHideAddress ? "Yes (through the relay, slower)" : "No"), 86.0f + 24.0f * 7.0f);
        label(r, bank, "Games on the local network:", 80, 284, 1, 1, 1);
        const auto& servers = browser_.servers();
        if (servers.empty()) label(r, bank, "(searching...)", 100, 306, 0.7f, 0.7f, 0.7f);
        for (std::size_t i = 0; i < servers.size() && i < 4; ++i) {
            static const char* const kPhase[4] = {"in the lobby", "playing", "playing", "playing"};
            const net::ServerInfo& info = servers[i].info;
            const float y = 306.0f + 22.0f * static_cast<float>(i);
            const bool on = row_ == kJoinFields + static_cast<int>(i);
            const std::string line = info.name + "   " + std::to_string(info.players) + " player" + (info.players == 1 ? "" : "s") + ", " +
                                     kPhase[static_cast<int>(info.phase)] + (info.password ? "   (password)" : "") +
                                     (info.version != net::kProtocolVersion ? "   (other version)" : "");
            label(r, bank, line, 100, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
            if (on) r.sprite(bank, "cursor1", frame / 8, -1, 82.0f, y + 15.0f);
        }
        item(kJoinFields + static_cast<int>(servers.size()), "Back", 404);
        label(r, bank, editing_ != nullptr ? "Type, then Enter" : "Up/Down: select   Enter: change / join   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    } else {
        fieldRow(0, "Your name:", &cfg_.netName, false, "Player");
        fieldRow(1, "Game name:", &cfg_.netServerName, false, "");
        fieldRow(2, "Port:", &portText_, false, "");
        fieldRow(3, "Password:", &password_, true, "(none)");
        {
            // A choice, changed with left and right (or by choosing it).
            const float y = 86.0f + 24.0f * static_cast<float>(kHostOnlineRow);
            const bool on = row_ == kHostOnlineRow;
            label(r, bank, "Announce online:", 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
            label(r, bank, kOnlineChoice[std::clamp(cfg_.netOnline, 0, 2)], 230, y, 1, 1, 1);
            if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
        }
        item(kHostStartRow, "Start the server", 86.0f + 24.0f * static_cast<float>(kHostStartRow) + 6.0f);
        item(kHostStartRow + 1, "Back", 86.0f + 24.0f * static_cast<float>(kHostStartRow + 1) + 6.0f);
        const std::vector<std::string> notes = {"Others join with your address and this port (TCP and UDP).",
                                                "On the same network they find the game by themselves.",
                                                "If your router lets nothing in: announce the game online, and give out its code.",
                                                "For a server without a window run atomic_server."};
        for (std::size_t i = 0; i < notes.size(); ++i) label(r, bank, notes[i], 60, 300.0f + 22.0f * static_cast<float>(i), 0.75f, 0.75f, 0.75f);
        label(r, bank, editing_ != nullptr ? "Type, then Enter" : "Up/Down: select   Enter: change / start   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
    }
}

// The games the master server lists.
void NetUi::drawBrowse(Renderer& r, SpriteBank& bank, int frame) {
#ifdef AB_ONLINE
    r.image(bank.picture("glue0"));
    r.quad(40, 40, 560, 400, 0.0f, 0.0f, 0.10f, 0.85f);
    label(r, bank, "Online Games", 55, 50, 1.0f, 0.95f, 0.3f);
    const auto& games = online_.lobbies();
    const int count = static_cast<int>(games.size());
    const std::string status = online_.busy() ? "Asking the master server..."
                               : !online_.error().empty() ? online_.error()
                               : count == 0 ? "No games are open just now."
                               : std::to_string(online_.lobbiesTotal()) + (online_.lobbiesTotal() == 1 ? " game" : " games");
    const std::vector<std::string> statusLines = wrap(r, bank, status, 520);
    for (std::size_t i = 0; i < statusLines.size() && i < 2; ++i) label(r, bank, statusLines[i], 60, 74.0f + 18.0f * static_cast<float>(i), 0.6f, 0.8f, 1.0f);
    // A window of the list that keeps the cursor in sight.
    const int first = std::clamp(row_ - kBrowseShown + 1, 0, std::max(0, count - kBrowseShown));
    for (int i = first; i < count && i < first + kBrowseShown; ++i) {
        const online::LobbyInfo& g = games[static_cast<std::size_t>(i)];
        const float y = 116.0f + 22.0f * static_cast<float>(i - first);
        const bool on = row_ == i;
        const bool other = g.version != net::kProtocolVersion;
        const float lit = other ? 0.55f : 0.85f;
        label(r, bank, wrap(r, bank, g.name, 200).front(), 80, y, on ? 1.0f : lit, on ? 0.95f : lit, on ? 0.3f : lit);
        label(r, bank, std::to_string(g.players) + "/" + std::to_string(g.seats), 290, y, lit, lit, lit);
        label(r, bank, other ? "other version" : g.phase == "lobby" ? "in the lobby" : "playing", 340, y, lit, lit, lit);
        label(r, bank, wrap(r, bank, g.level, 130).front(), 450, y, lit, lit, lit);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    }
    auto item = [&](int row, const std::string& text, float y) {
        const bool on = row == row_;
        label(r, bank, text, 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    };
    item(count, "Refresh", 388);
    item(count + 1, "Back", 412);
    label(r, bank, "Up/Down: select   Enter: join   Esc: back", 60, 448, 0.4f, 1.0f, 1.0f);
#else
    (void)r, (void)bank, (void)frame;
#endif
}

// A tournament: the file, what it says for this player, and its matches.
void NetUi::drawTournament(Renderer& r, SpriteBank& bank, int frame) {
    r.image(bank.picture("glue0"));
    r.quad(40, 40, 560, 400, 0.0f, 0.0f, 0.10f, 0.85f);
    label(r, bank, "Tournament", 55, 50, 1.0f, 0.95f, 0.3f);
    const bool typing = editing_ == &cfg_.tournament;
    const bool blink = (frame / 20) % 2 == 0;
    auto item = [&](int row, const std::string& text, float y, bool enabled = true) {
        const bool on = row == row_ && !typing;
        const float lit = enabled ? 0.85f : 0.45f;
        label(r, bank, text, 80, y, on ? 1.0f : lit, on ? 0.95f : lit, on ? 0.3f : lit);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    };
    item(0, "File or address:", 80);
    std::string shown = cfg_.tournament.empty() && !typing ? "(a path, or https://...)" : cfg_.tournament;
    while (r.textWidth(bank, shown) > 330 && shown.size() > 3) shown.erase(0, 1);  // the end stays visible
    label(r, bank, shown + (typing ? (blink ? "_" : " ") : ""), 230, 80, typing ? 0.4f : 1.0f, 1.0f, 1.0f);
    if (typing) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, 95.0f);
    const bool canPlay = tournamentLoaded_ && tournamentTrusted_ && tournamentNote_.find(": match ") != std::string::npos && tournamentNote_.find("waits") == std::string::npos;
    item(1, tournamentLoaded_ ? "Read it again" : "Open", 104);
    item(2, "Play my match", 128, canPlay);
    item(3, "This is my organiser's key", 152, tournamentLoaded_ && !tournamentTrusted_);
    item(4, "Back", 176);
    const std::vector<std::string> note = wrap(r, bank, tournamentNote_, 520);
    for (std::size_t i = 0; i < note.size() && i < 2; ++i) label(r, bank, note[i], 60, 206.0f + 18.0f * static_cast<float>(i), 0.4f, 1.0f, 1.0f);
#ifdef AB_ONLINE
    if (tournamentLoaded_) {
        const std::string title = tournament_.name + (tournament_.organiser.empty() ? "" : "  (" + tournament_.organiser + ")") + (tournamentTrusted_ ? "" : "  - not yet trusted");
        label(r, bank, wrap(r, bank, title, 520).front(), 60, 250, 1.0f, 0.95f, 0.3f);
        // The matches still to be played first, then the ones that are over; as many as fit.
        std::vector<const online::TournamentMatch*> order;
        for (const online::TournamentMatch& m : tournament_.matches)
            if (m.winner.empty()) order.push_back(&m);
        for (const online::TournamentMatch& m : tournament_.matches)
            if (!m.winner.empty()) order.push_back(&m);
        for (std::size_t i = 0; i < order.size() && i < 8; ++i) {
            const online::TournamentMatch& m = *order[i];
            std::string who;
            for (const std::string& p : m.players) who += (who.empty() ? "" : " - ") + (p[0] == '@' ? "winner of " + p.substr(1) : p);
            const float y = 272.0f + 18.0f * static_cast<float>(i);
            const float lit = m.winner.empty() ? 0.9f : 0.6f;
            label(r, bank, m.id, 60, y, lit, lit, lit);
            label(r, bank, wrap(r, bank, who, 300).front(), 130, y, lit, lit, lit);
            label(r, bank, wrap(r, bank, m.winner.empty() ? m.server : "won by " + m.winner, 150).front(), 440, y, lit, lit, lit);
        }
        if (order.size() > 8) label(r, bank, "(and " + std::to_string(order.size() - 8) + " more)", 60, 272.0f + 18.0f * 8.0f, 0.6f, 0.6f, 0.6f);
    }
#endif
    label(r, bank, typing ? "Type, then Enter" : "Up/Down: select   Enter: choose   Esc: back", 60, 448, 0.4f, 1.0f, 1.0f);
}

// Joining a game by the six digits its host gives out.
void NetUi::drawCode(Renderer& r, SpriteBank& bank, int frame) {
    r.image(bank.picture("glue0"));
    r.quad(40, 40, 560, 380, 0.0f, 0.0f, 0.10f, 0.85f);
    label(r, bank, "Join Online By Code", 55, 50, 1.0f, 0.95f, 0.3f);
    const bool typing = editing_ == &codeText_;
    const bool blink = (frame / 20) % 2 == 0;
    auto item = [&](int row, const std::string& text, float y) {
        const bool on = row == row_ && !typing;
        label(r, bank, text, 80, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
        if (on) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, y + 15.0f);
    };
    item(0, "Code:", 96);
    label(r, bank, codeText_ + (typing ? (blink ? "_" : " ") : codeText_.empty() ? "(six digits)" : ""), 230, 96, typing ? 0.4f : 1.0f, 1.0f, 1.0f);
    if (typing) r.sprite(bank, "cursor1", frame / 8, -1, 62.0f, 111.0f);
    item(1, "Join", 132);
    item(2, "Back", 156);
    label(r, bank, "The host of a game sees its code in the lobby.", 60, 200, 0.75f, 0.75f, 0.75f);
    label(r, bank, typing ? "Type the code, then Enter" : "Up/Down: select   Enter: choose   Esc: back", 60, 440, 0.4f, 1.0f, 1.0f);
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
                // Ready (the administrator's start says it for them): a green mark before the seat number.
                const bool ready = info->ready || seat.client == lobby.admin;
                if (!waiting) r.quad(17, y + 6, 5, 5, ready ? 0.2f : 0.35f, ready ? 0.9f : 0.35f, ready ? 0.3f : 0.35f, 1.0f);
                // How it is connected, unless straight: through the relay, or inside TCP.
                const std::string ping = (info->path != net::kPathDirect ? std::string(net::pathName(info->path)) + "  " : std::string()) + std::to_string(info->pingMs) + "ms";
                label(r, bank, ping, 304.0f - r.textWidth(bank, ping), y, 0.6f, 0.6f, 0.6f);
            }
    }
    std::string watching;
    for (const net::ClientInfo& c : lobby.clients)
        if (c.seat < 0) watching += (watching.empty() ? "Watching: " : ", ") + c.name;
    if (!watching.empty()) label(r, bank, wrap(r, bank, watching, 280).front(), 24, 260, 0.7f, 0.7f, 0.7f);
    label(r, bank, "* administrator   green mark: ready", 24, 278, 0.6f, 0.6f, 0.6f);
    if (!verifiedOperator_.empty()) {
        // A certificate that holds: who answers for this server.
        label(r, bank, wrap(r, bank, "verified server of " + verifiedOperator_, 284).front(), 24, 296, 0.3f, 0.95f, 0.4f);
    } else if (!certificateProblem_.empty()) {
        label(r, bank, wrap(r, bank, "certificate: " + certificateProblem_, 284).front(), 24, 296, 1.0f, 0.6f, 0.2f);
    } else if (!client_.serverIdentity().empty()) {
        // On a line of its own: beside the note above the two ran into each other.
        label(r, bank, "server id " + client_.serverIdentity(), 24, 296, 0.6f, 0.6f, 0.6f);
    }

    // The game's name, and for a game announced online its standing there (the code to give out).
    const float noteWidth = onlineNote_.empty() ? 0.0f : r.textWidth(bank, onlineNote_) + 12.0f;
    label(r, bank, wrap(r, bank, client_.serverName(), 284.0f - noteWidth).front(), 332, 16, 1.0f, 0.95f, 0.3f);
    if (!onlineNote_.empty()) label(r, bank, onlineNote_, 618.0f - r.textWidth(bank, onlineNote_), 16, 0.4f, 1.0f, 1.0f);
    // What the lobby is waiting for, then the settings.
    {
        const net::ClientInfo* a = lobby.client(lobby.admin);
        const std::string adminName = a != nullptr ? a->name : std::string("the administrator");
        std::string missing;
        for (const net::ClientInfo& c : lobby.clients)
            if (c.seat >= 0 && c.id != lobby.admin && !c.ready) missing += (missing.empty() ? "" : ", ") + c.name;
        const std::string status = waiting ? (client_.sittingOut() ? "The match goes on without you" : "A match is being played")
                                   : !missing.empty() && admin ? "Not ready yet: " + missing
                                   : admin ? "Everybody is ready"
                                   : !client_.ready() && client_.seat() >= 0 ? "Say when you are ready"
                                   : !missing.empty() ? "Waiting for: " + missing
                                   : "Waiting for " + adminName + " to start";
        label(r, bank, wrap(r, bank, status, 284).front(), 332, 40, 0.4f, 1.0f, 1.0f);
    }
    for (int row = 0; row < kLobbySettings; ++row) {
        const float y = 58.0f + 18.0f * static_cast<float>(row);
        const bool on = row == lobbyRow_;
        const std::string line = wrap(r, bank, optionText(lobby.settings, row), 270).front();
        const float dim = admin ? 1.0f : 0.75f;
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

    // The row of actions; the one chosen is on a lighter ground.
    if (escapeAt_ != 0 && nowMs - escapeAt_ < 3000) {
        label(r, bank, "Press Esc again to leave the server", 24, 458, 0.4f, 1.0f, 1.0f);
        return;
    }
    const std::vector<LobbyItem> items = lobbyItems();
    float x = 24.0f;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const LobbyItem& it = items[i];
        const bool on = lobbyRow_ == kLobbySettings && static_cast<int>(i) == std::clamp(lobbyAction_, 0, static_cast<int>(items.size()) - 1);
        const float w = r.textWidth(bank, it.label);
        if (on) r.quad(x - 6.0f, 456.0f, w + 12.0f, 20.0f, 0.25f, 0.30f, 0.75f, 1.0f);
        const float lit = it.enabled ? 1.0f : 0.5f;
        label(r, bank, it.label, x, 458, on ? 1.0f : 0.85f * lit, on ? 0.95f * lit : 0.85f * lit, on ? 0.3f : 0.85f * lit);
        x += w + 22.0f;
    }
}

// The on-screen keyboard, over the lower part of whatever screen asked for it. Its first
// line is the text being typed, since it may cover the place the text is shown at.
void NetUi::drawKeyboard(Renderer& r, SpriteBank& bank, int frame) {
    if (!osk_.shown()) return;
    const float cellW = 50.0f, cellH = 28.0f, left = 70.0f;
    const float top = 474.0f - 22.0f - cellH * static_cast<float>(osk_.rows()) - 30.0f;
    r.quad(60, top, 520, 474.0f - top, 0.0f, 0.0f, 0.10f, 1.0f);
    const bool secret = editing_ == &password_;
    const std::string& value = editing_ != nullptr ? *editing_ : chatText_;
    std::string typing = "> " + (secret ? std::string(value.size(), '*') : value);
    while (r.textWidth(bank, typing) > 480 && typing.size() > 3) typing.erase(2, 1);  // the end stays visible
    label(r, bank, typing + ((frame / 20) % 2 == 0 ? "_" : ""), left, top + 6.0f, 0.4f, 1.0f, 1.0f);
    for (const ScreenKeyboard::Cell& c : osk_.cells()) {
        const float x = left + cellW * static_cast<float>(c.col), y = top + 30.0f + cellH * static_cast<float>(c.row);
        const float w = cellW * static_cast<float>(c.span) - 4.0f;
        if (c.selected) r.quad(x, y, w, cellH - 4.0f, 0.25f, 0.30f, 0.75f, 1.0f);
        else r.quad(x, y, w, cellH - 4.0f, 0.12f, 0.12f, 0.28f, 1.0f);
        label(r, bank, c.label, x + (w - r.textWidth(bank, c.label)) / 2.0f, y + 3.0f, c.selected ? 1.0f : 0.9f, c.selected ? 0.95f : 0.9f, c.selected ? 0.3f : 0.9f);
    }
    label(r, bank, "A: press   X: erase   Y: space   Start: done   B: close", left, 474.0f - 22.0f, 0.6f, 0.6f, 0.6f);
}

// The menu of a match: the game goes on behind it.
void NetUi::drawMatchMenu(Renderer& r, SpriteBank& bank) {
    if (!menuOpen_) return;
    r.quad(200, 150, 240, 44.0f + 26.0f * kMenuItems, 0.0f, 0.0f, 0.10f, 0.92f);
    for (int i = 0; i < kMenuItems; ++i) {
        const bool on = i == menuItem_;
        const float y = 166.0f + 26.0f * static_cast<float>(i);
        if (on) r.quad(212, y - 2.0f, 216, 22, 0.25f, 0.30f, 0.75f, 1.0f);
        label(r, bank, kMenuItem[i], 320.0f - r.textWidth(bank, kMenuItem[i]) / 2.0f, y, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
    }
}

void NetUi::drawChat(Renderer& r, SpriteBank& bank, std::uint64_t nowMs) {
    std::vector<std::pair<std::string, bool>> lines;
    for (const net::ChatLine& c : client_.chat()) {
        if (!chatOpen_ && nowMs - c.atMs > kChatShownMs) continue;
        for (const std::string& part : wrap(r, bank, c.fromServer ? "* " + c.text : c.name + ": " + c.text, 560)) lines.emplace_back(part, c.fromServer);
    }
    const std::size_t shown = std::min<std::size_t>(lines.size(), chatOpen_ ? 6 : 4);
    const float bottom = osk_.shown() ? 200.0f : 428.0f;  // the on-screen keyboard has the lower part
    if (chatOpen_) r.quad(20, bottom - 20.0f * static_cast<float>(shown) - 4.0f, 600, 20.0f * static_cast<float>(shown + 1) + 8.0f, 0.0f, 0.0f, 0.10f, 0.75f);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [text, fromServer] = lines[lines.size() - shown + i];
        label(r, bank, text, 28, bottom - 20.0f * static_cast<float>(shown - i), fromServer ? 0.6f : 1.0f, fromServer ? 0.8f : 1.0f, 1.0f);
    }
    if (chatOpen_ && !osk_.shown()) {
        std::string typing = "> " + chatText_;
        while (r.textWidth(bank, typing) > 570 && typing.size() > 3) typing.erase(2, 1);
        label(r, bank, typing + "_", 28, bottom, 0.4f, 1.0f, 1.0f);
    }
    if (menuOpen_ || osk_.shown()) return;
    if (escapeAt_ != 0 && nowMs - escapeAt_ < 3000) label(r, bank, "Press Esc again to go back to the lobby", 160, 456, 1.0f, 0.95f, 0.3f);
    else if (client_.seat() < 0 && client_.state() == State::Round) label(r, bank, "Watching - you play in the next match   T: chat", 150, 456, 0.8f, 0.8f, 0.8f);
}

void NetUi::drawRoulette(Renderer& r, SpriteBank& bank) {
    static const char* const kPower[kPowTypeCount] = {"bomb", "flame", "disease", "kicker", "skate", "punch", "grab", "spooge",
                                                      "goldflame", "trigger", "jelly", "disease3", "random", "clog", "clog"};
    static const char* const kPrizeText[kPowTypeCount] = {
        "an extra bomb", "longer flame length", "a disease", "the ability to kick bombs", "extra speed",
        "the ability to punch bombs", "the ability to grab bombs", "the spooger", "goldflame", "a trigger mechanism",
        "jelly (bouncy) bombs", "super bad disease", "random", "a speed brake (slowness)", ""};
    const net::RouletteMsg& show = *client_.roulette();
    r.image(bank.picture("roulette"));
    float x = 0, y = 0;
    for (int s = 0; s < Roulette::kSlots; ++s) {
        wheel_->screenPosition(wheel_->slotPosition(s), &x, &y);
        r.sprite(bank, std::string("power ") + kPower[Roulette::kPrize[static_cast<std::size_t>(s)]], 0, -1, x, y);
    }
    wheel_->screenPosition(wheel_->pointer(), &x, &y);
    r.sprite(bank, "ring", 0, -1, x, y);
    const std::string who = show.teamPlay ? "Team " + std::to_string(show.winner + 1) : client_.lobby().seatName(show.winner);
    const std::string top = "The wheel turns for " + (who.empty() ? std::string("the Gold Player") : who);
    label(r, bank, top, 320.0f - r.textWidth(bank, top) / 2.0f, 100, 1, 1, 1);
    if (wheel_->state() == Roulette::State::Stopped && show.prize >= 0) {
        // The server's word on the prize (it is its wheel that counts).
        const std::string lines[3] = {"The Gold Player has", kPrizeText[show.prize], "for the next match!!"};
        for (int i = 0; i < 3; ++i) label(r, bank, lines[i], 320.0f - r.textWidth(bank, lines[i]) / 2.0f, 220.0f + 20.0f * static_cast<float>(i), 1.0f, 0.95f, 0.3f);
    }
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
    if (menuOpen_ || osk_.shown()) return;
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
        drawMatchMenu(r, bank);
        drawKeyboard(r, bank, frame);
        r.end();
        return;
    }
    r.begin(windowW, windowH);
    if (mode_ == Mode::Join || mode_ == Mode::Host) {
        drawEntry(r, bank, frame);
    } else if (mode_ == Mode::Browse) {
        drawBrowse(r, bank, frame);
    } else if (mode_ == Mode::Code) {
        drawCode(r, bank, frame);
    } else if (mode_ == Mode::Tournament) {
        drawTournament(r, bank, frame);
    } else if (mode_ == Mode::Session && client_.roulette() != nullptr && wheel_) {
        drawRoulette(r, bank);
        drawChat(r, bank, nowMs);
    } else if (mode_ == Mode::Session) {
        drawLobby(r, bank, frame, nowMs);
    } else {
        r.image(bank.picture("glue0"));
        std::vector<std::string> lines;
        if (mode_ == Mode::Error) {
            lines = wrap(r, bank, error_, 400);
            lines.insert(lines.begin(), "Network game");
        } else {
            lines = {"Network game", hosting_ ? "Starting the server..." : !connectingText_.empty() ? connectingText_ : "Connecting to " + connectingTo_ + "..."};
        }
        const float boxH = 70.0f + 24.0f * static_cast<float>(lines.size());
        r.quad(100, 180, 440, boxH, 0.0f, 0.0f, 0.10f, 0.88f);
        for (std::size_t i = 0; i < lines.size(); ++i)
            label(r, bank, lines[i], 320.0f - r.textWidth(bank, lines[i]) / 2.0f, 196.0f + 24.0f * static_cast<float>(i), i == 0 ? 1.0f : 0.9f, i == 0 ? 0.95f : 0.9f, i == 0 ? 0.3f : 0.9f);
        // The items of the box: the chosen one on a lighter ground.
        std::vector<std::string> items = {mode_ == Mode::Error ? "Ok" : "Cancel"};
        if (mode_ == Mode::Error && identityChanged_) items.push_back("Forget the old identity");
        float total = 30.0f * static_cast<float>(items.size() - 1);
        for (const std::string& it : items) total += r.textWidth(bank, it);
        float x = 320.0f - total / 2.0f;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool on = static_cast<int>(i) == (mode_ == Mode::Error ? errorItem_ : 0);
            const float w = r.textWidth(bank, items[i]);
            if (on) r.quad(x - 6.0f, 180.0f + boxH - 32.0f, w + 12.0f, 20.0f, 0.25f, 0.30f, 0.75f, 1.0f);
            label(r, bank, items[i], x, 180.0f + boxH - 30.0f, on ? 1.0f : 0.85f, on ? 0.95f : 0.85f, on ? 0.3f : 0.85f);
            x += w + 30.0f;
        }
    }
    drawKeyboard(r, bank, frame);
    r.end();
}

}  // namespace ab
