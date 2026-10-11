// Screens of the network mode (docs/specifications/networking.md): joining and
// hosting, the lobby with its chat, and the match as the server runs it.
// "Start Network Game" and "Join Network Game" of the main menu lead here.
// The layout and texts are this implementation's; the original's network
// screens are not reproduced.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <memory>

#include "app/pad_keys.hpp"
#include "app/screen_keyboard.hpp"
#include "game/roulette.hpp"
#include "net/client.hpp"
#include "net/server.hpp"
#ifdef AB_ONLINE
#include "online/announcer.hpp"
#include "online/bootstrap.hpp"
#endif
#include "rendering/renderer.hpp"
#include "rendering/sprites.hpp"
#include "resources/settings.hpp"

namespace ab {

class NetUi {
public:
    // What the screens need from the rest of the program.
    struct Hooks {
        std::function<void(int level)> roundStarted;                 // load the level's graphics, play its music
        std::function<void(const World&, const std::vector<Event>&)> stepped;  // the sounds of a step
        std::function<void(bool draw, bool matchOver)> resultShown;  // the result tune and voice line
        std::function<void()> lobbyEntered;                          // the pre-game tune
        std::function<void(int id)> sound;                           // a menu sound (10 choose, 20 move, 40 refuse)
        std::function<void()> settingsChanged;                       // name, address etc. were edited: save them
    };

    NetUi(Settings& settings, std::string gameDir, std::string userSchemesDir, Hooks hooks);
    ~NetUi();

    void openJoin();
    void openHost();
    // For automated checks: host at once on this port; optionally start the match as soon as the lobby is up.
    void hostNow(int port, bool startMatch);
    // Join this address at once (the --connect option).
    void joinNow(const std::string& address);
    bool finished() const { return mode_ == Mode::Closed; }
    bool wantsTextInput() const;
    bool inRound() const;
    bool showingResult() const;

    // The gamepads steer players now (a round is on and no menu, chat line or keyboard is open).
    bool padsPlay() const;
    bool keyboardShown() const { return osk_.shown(); }
    const ScreenKeyboard& keyboard() const { return osk_; }
    bool matchMenuShown() const { return menuOpen_; }
    // For automated checks: the screen shown ("join", "host", "browse", "code", "connecting", "error",
    // "lobby", "wheel", "round", "result", "closed"), the connection, and a host that stays off the network
    // outside this machine (no router port, no announcements).
    const char* screenName() const;
    const net::Client& client() const { return client_; }
    void setLocalOnly(bool on) { localOnly_ = on; }
    // The configuration of online play named on the command line: where it is and the key
    // that must have signed it. Goes before the settings and before what is built in.
    void setOnlineConfig(const std::string& urls, const std::string& keyHex) {
        onlineUrls_ = urls;
        onlineKey_ = keyHex;
    }
    const std::string& errorText() const { return error_; }
    bool onlineBusy() const;  // the master server is being asked something
    // For automated checks: the hosted game's port and identity key (empty when not hosting).
    int hostPort() const { return hosting_ ? server_.port() : 0; }
    std::string hostIdentityKey() const { return hosting_ ? server_.identityKeyHex() : std::string(); }
    const std::string& onlineNote() const { return onlineNote_; }  // a hosted game's standing with the master server

    // The SDL keycode a gamepad's key is passed to key() as. (Start has a key of its own,
    // the "menu" key of a keyboard.)
    static unsigned padKeycode(PadKey k);
    // An SDL keycode. `pad`: pressed on a gamepad, not on a keyboard: a text field then
    // opens the on-screen keyboard.
    void key(unsigned key, bool ctrl, bool pad = false);
    void text(const char* utf8);        // typed characters, while wantsTextInput()
    // Network and round. `locals` are the controllers of the players at this computer:
    // the first and second key set (each with a gamepad), then two more gamepads.
    void update(std::uint64_t nowMs, const std::array<PlayerInput, net::kMaxLocalPlayers>& locals);
    void draw(Renderer& r, SpriteBank& bank, int windowW, int windowH, int frame, std::uint64_t nowMs);

private:
    enum class Mode { Closed, Join, Host, Connecting, Session, Error, Browse, Code };

    void close();
    void connectTo(const std::string& address, bool remember);
    void startHost();
    // Online play (the master server): see docs/specifications/online-play.md.
    bool onlineBegin();  // false: not possible, and the error screen says why
    void openBrowse();
    void openCode(bool pad);
    void askJoin(const std::string& lobbyId, const std::string& code);
    void updateOnline(std::uint64_t nowMs);
    void showError(const std::string& why);
    void drawBrowse(Renderer& r, SpriteBank& bank, int frame);
    void drawCode(Renderer& r, SpriteBank& bank, int frame);
    void leave();
    void submitChat();
    void edit(std::string* value, std::size_t maxLength, bool digitsOnly, bool pad);
    bool keyboardKey(unsigned key, bool pad);  // a key while the on-screen keyboard is shown; true: taken
    void finishKeyboard(bool submit);
    enum class LobbyAction { Start, Ready, Team, Players, Chat, Leave };
    struct LobbyItem {
        LobbyAction action;
        int local;  // Team: which of this computer's players
        std::string label;
        bool enabled;
    };
    std::vector<LobbyItem> lobbyItems() const;
    void lobbyAct(const LobbyItem& item, bool pad);
    void drawKeyboard(Renderer& r, SpriteBank& bank, int frame);
    void drawMatchMenu(Renderer& r, SpriteBank& bank);
    std::uint64_t clock() const;
    bool escape(std::uint64_t nowMs);  // true on the second press
    std::string playerName() const;
    void label(Renderer& r, SpriteBank& bank, const std::string& s, float x, float y, float red, float green, float blue) const;
    void drawEntry(Renderer& r, SpriteBank& bank, int frame);
    void drawLobby(Renderer& r, SpriteBank& bank, int frame, std::uint64_t nowMs);
    void drawResult(Renderer& r, SpriteBank& bank);
    void drawRoulette(Renderer& r, SpriteBank& bank);
    void drawChat(Renderer& r, SpriteBank& bank, std::uint64_t nowMs);
    std::vector<std::string> wrap(Renderer& r, SpriteBank& bank, const std::string& s, float width) const;

    Settings& cfg_;
    std::string gameDir_;
    std::string userSchemesDir_;
    Hooks hooks_;

    Mode mode_ = Mode::Closed;
    Mode entry_ = Mode::Join;  // the screen an error or Esc returns to
    int row_ = 0;
    std::string* editing_ = nullptr;
    std::size_t editMax_ = 0;
    bool editDigits_ = false;
    std::string password_;
    std::string portText_;
    std::string error_;
    std::string connectingTo_;
    bool identityChanged_ = false;  // the error shown is a changed server identity

    net::Client client_;
    net::Server server_;
    bool hosting_ = false;
    net::LanBrowser browser_;
    std::uint64_t now_ = 0;
    net::Client::State lastState_ = net::Client::State::Idle;
    std::uint32_t lastRound_ = 0;
    bool autoStart_ = false;

    int lobbyRow_ = 0;           // a setting, or the row of actions below them
    int lobbyAction_ = 0;        // the action selected in that row
    ScreenKeyboard osk_;
    bool menuOpen_ = false;      // the menu of a match (Start on a gamepad)
    int menuItem_ = 0;
    int errorItem_ = 0;          // 0 Ok, 1 forget the old identity
    int shownServers_ = 0;       // servers listed at the last update
    std::uint64_t connectingSince_ = 0;
    bool localOnly_ = false;
    std::string onlineUrls_, onlineKey_;  // from the command line
    std::string onlineName_;              // the name the session with the master was opened under
    std::string codeText_;                // the join code being typed
    enum class Asking { Nothing, Find, Join } asking_ = Asking::Nothing;  // what the master is being asked
    std::vector<std::string> candidates_; // the ways to the game server the master named
    std::size_t candidate_ = 0;           // the one being tried
    std::string expectIdentity_;          // the identity that server must show
    bool hostOnline_ = false;             // the hosted game is announced to the master
    std::string onlineNote_;
#ifdef AB_ONLINE
    online::Online online_;
    online::HostAnnouncer announcer_;
#endif
    std::string chatText_;
    bool chatOpen_ = false;      // the chat line during a match
    bool continueSent_ = false;
    std::uint64_t escapeAt_ = 0; // first Esc: a second one within three seconds leaves
    RenderSnapshot previous_;
    Values wheelValues_ = Values::defaults();
    std::unique_ptr<Roulette> wheel_;  // the bonus wheel while the server shows it
    std::uint32_t wheelSeed_ = 0;
    int wheelFrames_ = 0;
};

}  // namespace ab
