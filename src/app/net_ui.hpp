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

#include "net/client.hpp"
#include "net/server.hpp"
#include "rendering/renderer.hpp"
#include "rendering/sprites.hpp"
#include "resources/settings.hpp"

namespace ab {

class NetUi {
public:
    // What the screens need from the rest of the program.
    struct Hooks {
        std::function<void(int level)> roundStarted;                 // load the level's graphics, play its music
        std::function<void(World&)> stepped;                         // the sounds of a step
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

    void key(unsigned key, bool ctrl);  // an SDL keycode
    void text(const char* utf8);        // typed characters, while wantsTextInput()
    // Network and round. `local` is this player's controller.
    void update(std::uint64_t nowMs, const PlayerInput& local);
    void draw(Renderer& r, SpriteBank& bank, int windowW, int windowH, int frame, std::uint64_t nowMs);

private:
    enum class Mode { Closed, Join, Host, Connecting, Session, Error };

    void close();
    void connectTo(const std::string& address, bool remember);
    void startHost();
    void leave();
    void submitChat();
    void edit(std::string* value, std::size_t maxLength, bool digitsOnly);
    bool escape(std::uint64_t nowMs);  // true on the second press
    std::string playerName() const;
    void label(Renderer& r, SpriteBank& bank, const std::string& s, float x, float y, float red, float green, float blue) const;
    void drawEntry(Renderer& r, SpriteBank& bank, int frame);
    void drawLobby(Renderer& r, SpriteBank& bank, int frame, std::uint64_t nowMs);
    void drawResult(Renderer& r, SpriteBank& bank);
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

    net::Client client_;
    net::Server server_;
    bool hosting_ = false;
    net::LanBrowser browser_;
    std::uint64_t now_ = 0;
    net::Client::State lastState_ = net::Client::State::Idle;
    std::uint32_t lastRound_ = 0;
    bool autoStart_ = false;

    int lobbyRow_ = 0;
    std::string chatText_;
    bool chatOpen_ = false;      // the chat line during a match
    bool continueSent_ = false;
    std::uint64_t escapeAt_ = 0; // first Esc: a second one within three seconds leaves
    RenderSnapshot previous_;
};

}  // namespace ab
