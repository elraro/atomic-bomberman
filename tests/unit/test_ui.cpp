// Tests of the network screens as a gamepad works them (docs/specifications/
// online-play.md, phase 1): the pad's keys, the on-screen keyboard, and a host
// and a guest over 127.0.0.1 doing everything with pad keys alone. The screens
// are never drawn, so there is no window; time is simulated.
#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "app/net_ui.hpp"
#include "app/pad_keys.hpp"
#include "app/screen_keyboard.hpp"
#include "net/server.hpp"
#include "net/socket.hpp"

using namespace ab;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_TEXT(a, b)                                                                                              \
    do {                                                                                                              \
        ++g_checks;                                                                                                   \
        const std::string va = (a);                                                                                   \
        const std::string vb = (b);                                                                                   \
        if (va != vb) {                                                                                               \
            ++g_failures;                                                                                             \
            std::printf("  FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, va.c_str(), vb.c_str()); \
        }                                                                                                             \
    } while (0)

using Keys = std::vector<PadKey>;

void testPadKeys() {
    PadKeys pad;
    using B = PadKeys::Button;
    // A press is a key; holding a direction repeats it, first after 400 ms, then every 130.
    pad.button(B::Down, true, 1000);
    CHECK(pad.take() == Keys{PadKey::Down});
    pad.update(1399);
    CHECK(pad.take().empty());
    pad.update(1400);
    CHECK(pad.take() == Keys{PadKey::Down});
    pad.update(1529);
    CHECK(pad.take().empty());
    pad.update(1530);
    CHECK(pad.take() == Keys{PadKey::Down});
    pad.button(B::Down, false, 1600);
    pad.update(3000);
    CHECK(pad.take().empty());
    // The face buttons do not repeat; releasing is nothing.
    pad.button(B::South, true, 3000);
    pad.button(B::South, false, 3010);
    pad.button(B::East, true, 3020);
    pad.button(B::West, true, 3030);
    pad.button(B::North, true, 3040);
    pad.button(B::Start, true, 3050);
    pad.update(5000);
    CHECK((pad.take() == Keys{PadKey::Confirm, PadKey::Back, PadKey::Erase, PadKey::Space, PadKey::Menu}));

    // The stick: a direction from half deflection, the stronger axis wins, and it is
    // over only when the stick is nearly back in the middle.
    pad.stick(0, 10000, 0, 6000);
    CHECK(pad.take().empty());
    pad.stick(0, 20000, 8000, 6010);
    CHECK(pad.take() == Keys{PadKey::Right});
    pad.stick(0, 12000, 0, 6020);  // between the two thresholds: still right
    pad.update(6410);
    CHECK(pad.take() == Keys{PadKey::Right});
    pad.stick(0, 0, -30000, 6420);
    CHECK(pad.take() == Keys{PadKey::Up});
    pad.stick(0, 0, 0, 6430);
    pad.update(9000);
    CHECK(pad.take().empty());
    // A second pad's stick counts too.
    pad.stick(1, -32768, 0, 9000);
    CHECK(pad.take() == Keys{PadKey::Left});
    pad.stick(1, 0, 0, 9010);
    CHECK(pad.take().empty());

    // While the players steer, only Start is a key, and a direction held when the round
    // ends does not start repeating.
    pad.setPlaying(true);
    pad.button(B::South, true, 10000);
    pad.button(B::Left, true, 10000);
    pad.stick(0, 32767, 0, 10000);
    pad.update(11000);
    CHECK(pad.take().empty());
    pad.button(B::Start, true, 11000);
    CHECK(pad.take() == Keys{PadKey::Menu});
    pad.setPlaying(false);
    pad.update(13000);
    CHECK(pad.take().empty());
    pad.button(B::Left, false, 13000);
    pad.button(B::Left, true, 13010);
    CHECK(pad.take() == Keys{PadKey::Left});
}

// The label of the selected key.
std::string selected(const ScreenKeyboard& k) {
    for (const ScreenKeyboard::Cell& c : k.cells())
        if (c.selected) return c.label;
    return {};
}

void testScreenKeyboard() {
    ScreenKeyboard k;
    CHECK(!k.shown());
    CHECK(k.confirm().action == ScreenKeyboard::Action::None);
    k.open(false);
    CHECK(k.shown());
    CHECK_TEXT(selected(k), "q");
    CHECK_TEXT(k.confirm().text, "q");
    k.move(1);
    CHECK_TEXT(selected(k), "w");
    k.move(3), k.move(3);  // round the left edge
    CHECK_TEXT(selected(k), "p");
    k.move(0);
    CHECK_TEXT(selected(k), "0");
    k.move(0);  // round the top: the wide keys, the one under that column
    CHECK_TEXT(selected(k), "Done");
    CHECK(k.confirm().action == ScreenKeyboard::Action::Done);
    k.move(3);
    CHECK_TEXT(selected(k), "Erase");
    CHECK(k.confirm().action == ScreenKeyboard::Action::Erase);
    k.move(3);
    CHECK_TEXT(selected(k), "Space");
    CHECK_TEXT(k.confirm().text, " ");
    k.move(3);
    CHECK_TEXT(selected(k), "Shift");
    CHECK(k.confirm().action == ScreenKeyboard::Action::None);
    CHECK(k.shifted());
    CHECK_TEXT(selected(k), "SHIFT");
    k.move(3);  // round the edge again
    CHECK_TEXT(selected(k), "Done");
    k.move(1);
    CHECK_TEXT(selected(k), "SHIFT");
    k.move(2), k.move(2);  // round the bottom to the digits, then the letters, now capitals
    CHECK_TEXT(selected(k), "Q");
    CHECK_TEXT(k.confirm().text, "Q");
    // Every key is a cell exactly once, and exactly one is selected.
    int on = 0;
    for (const ScreenKeyboard::Cell& c : k.cells()) on += c.selected ? 1 : 0;
    CHECK(on == 1);
    CHECK(k.cells().size() == 54);
    CHECK(k.rows() == 6);

    // A field of digits: the digits, Erase and Done.
    k.open(true);
    CHECK(!k.shifted());
    CHECK(k.rows() == 2);
    CHECK(k.cells().size() == 12);
    CHECK_TEXT(selected(k), "1");
    k.move(2);
    CHECK_TEXT(selected(k), "Erase");
    k.move(1);
    CHECK_TEXT(selected(k), "Done");
    k.move(1);
    CHECK_TEXT(selected(k), "Erase");
    k.move(2);
    CHECK_TEXT(selected(k), "1");
    k.close();
    CHECK(!k.shown());
}

// One computer's screens, worked with a pad.
struct Station {
    Settings cfg;
    NetUi ui;
    Station() : ui(cfg, "", "", {}) { ui.setLocalOnly(true); }

    void press(PadKey k) { ui.key(NetUi::padKeycode(k), false, true); }
    void press(PadKey k, int times) {
        for (int i = 0; i < times; ++i) press(k);
    }
    // Moves over the on-screen keyboard to the key with this label, and presses it.
    bool pressKey(const std::string& label) {
        const ScreenKeyboard& k = ui.keyboard();
        for (int tries = 0; tries < 40 && k.shown(); ++tries) {
            int row = -1, target = -1;
            for (const ScreenKeyboard::Cell& c : k.cells()) {
                if (c.selected) row = c.row;
                if (c.label == label) target = c.row;
            }
            if (target < 0) return false;
            if (selected(k) == label) {
                press(PadKey::Confirm);
                return true;
            }
            press(row != target ? PadKey::Down : PadKey::Right);
        }
        return false;
    }
    bool type(const std::string& text) {
        for (char c : text) {
            if (c == ' ') {
                press(PadKey::Space);  // the button for it
                continue;
            }
            const bool upper = c >= 'A' && c <= 'Z';
            if (upper != ui.keyboard().shifted() && !pressKey(ui.keyboard().shifted() ? "SHIFT" : "Shift")) return false;
            if (!pressKey(std::string(1, c))) return false;
        }
        return true;
    }
    std::string screen() const { return ui.screenName(); }
};

struct Table {
    Station host, guest;
    std::uint64_t now = net::clockMs();  // from the real time on, in steps of its own
    Table() { turn(); }  // the screens take their time from the updates
    void turn() {
        const std::array<PlayerInput, net::kMaxLocalPlayers> idle{};
        host.ui.update(now, idle);
        guest.ui.update(now, idle);
        now += 5;
        std::this_thread::yield();
    }
    bool until(const std::function<bool()>& done, int maxMs = 20000) {
        for (int waited = 0; waited < maxMs; waited += 5) {
            if (done()) return true;
            turn();
        }
        return done();
    }
    void spin(int ms) {
        for (int waited = 0; waited < ms; waited += 5) turn();
    }
};

// A port nothing listens on: the system picks one for a server that is stopped again.
int freePort() {
    net::Server probe;
    net::ServerConfig config;
    config.port = 0;
    config.discoverable = false;
    if (!probe.start(config)) return 27444;
    const int port = probe.port();
    probe.stop();
    return port;
}

bool chatHas(const NetUi& ui, const std::string& text) {
    for (const net::ChatLine& line : ui.client().chat())
        if (line.text == text) return true;
    return false;
}

// Host and guest, from the first screen to a running match and out again, with pad keys only.
void testScreensByPad() {
    Table t;
    Station& host = t.host;
    Station& guest = t.guest;
    const std::string port = std::to_string(freePort());

    // --- hosting: name, port, start ---
    host.ui.openHost();
    CHECK_TEXT(host.screen(), "host");
    host.press(PadKey::Up, 5);  // from "Start the server" to the name
    host.press(PadKey::Confirm);
    CHECK(host.ui.keyboardShown());
    host.press(PadKey::Erase, 40);
    CHECK(host.type("Ann"));
    host.press(PadKey::Menu);  // Start: done
    CHECK(!host.ui.keyboardShown());
    CHECK_TEXT(host.cfg.netName, "Ann");
    host.press(PadKey::Down, 2);  // the port
    host.press(PadKey::Confirm);
    CHECK(host.ui.keyboardShown());
    CHECK(host.ui.keyboard().rows() == 2);  // digits only
    host.press(PadKey::Erase, 5);
    CHECK(host.type(port));
    CHECK(host.pressKey("Done"));
    CHECK(!host.ui.keyboardShown());
    host.press(PadKey::Down, 3);  // password, relay, start
    host.press(PadKey::Confirm);
    CHECK(t.until([&] { return host.screen() == "lobby" && host.ui.client().lobby().clients.size() == 1; }));
    if (host.screen() != "lobby") return;
    CHECK(host.ui.client().isAdmin());
    CHECK_TEXT(host.ui.client().lobby().clients.front().name, "Ann");

    // --- joining: name, address, "Join this address" ---
    guest.ui.openJoin();
    CHECK_TEXT(guest.screen(), "join");
    guest.press(PadKey::Up);  // from the address to the name
    guest.press(PadKey::Confirm);
    guest.press(PadKey::Erase, 40);
    CHECK(guest.type("Bob B"));
    guest.press(PadKey::Back);  // B closes the keyboard; the field keeps the text
    CHECK(!guest.ui.keyboardShown());
    CHECK_TEXT(guest.cfg.netName, "Bob B");
    CHECK_TEXT(guest.screen(), "join");
    guest.press(PadKey::Down);
    guest.press(PadKey::Confirm);
    guest.press(PadKey::Erase, 70);
    CHECK(guest.type("127.0.0.1:" + port));
    CHECK(guest.pressKey("Erase"));  // the key, as well as the button
    CHECK(guest.type(port.substr(port.size() - 1)));
    guest.press(PadKey::Menu);
    CHECK_TEXT(guest.cfg.netAddress, "127.0.0.1:" + port);
    CHECK_TEXT(guest.screen(), "join");  // "done" on the keyboard does not join by itself
    guest.press(PadKey::Down, 2);  // password, then "Join this address"
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.screen() == "lobby" && host.ui.client().lobby().clients.size() == 2 && guest.ui.client().seat() >= 0; }));
    if (guest.screen() != "lobby") return;
    const std::uint8_t bob = guest.ui.client().id();

    // --- the lobby ---
    // The cursor starts on the first action: Start for the host, Ready for the guest.
    host.press(PadKey::Confirm);
    CHECK(t.until([&] { return chatHas(host.ui, "Not ready yet: Bob B"); }));
    CHECK_TEXT(host.screen(), "lobby");
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().ready(); }));
    guest.press(PadKey::Confirm);  // and taken back
    CHECK(t.until([&] { return !guest.ui.client().ready(); }));
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return host.ui.client().lobby().client(bob) != nullptr && host.ui.client().lobby().client(bob)->ready; }));

    // A setting, by the host: down from the actions to the first setting, on to Team Play.
    host.press(PadKey::Down, 4);
    host.press(PadKey::Right);
    CHECK(t.until([&] { return guest.ui.client().lobby().settings.teamPlay; }));
    // The guest may look at the settings but not change them.
    guest.press(PadKey::Down, 4);
    guest.press(PadKey::Right);
    guest.press(PadKey::Confirm);
    t.spin(200);
    CHECK(guest.ui.client().lobby().settings.teamPlay);
    // Start on the pad: back to the actions. With team play there is a Team action.
    guest.press(PadKey::Menu);
    const int seat = guest.ui.client().seat();
    CHECK(seat >= 0);
    const int team = guest.ui.client().lobby().seats[static_cast<std::size_t>(seat)].team;
    guest.press(PadKey::Right);  // Ready, Team
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().lobby().seats[static_cast<std::size_t>(seat)].team != team; }));
    guest.press(PadKey::Confirm);  // and back, so that both teams have somebody
    CHECK(t.until([&] { return guest.ui.client().lobby().seats[static_cast<std::size_t>(seat)].team == team; }));
    // Players at this computer: one more with each press, after four one again.
    guest.press(PadKey::Right);
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().localPlayers() == 2; }));
    // (The second player brought a second Team action, so Players moved one to the right.)
    guest.press(PadKey::Right);
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().localPlayers() == 3; }));
    guest.press(PadKey::Right);
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().localPlayers() == 4; }));
    guest.press(PadKey::Right);
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.ui.client().localPlayers() == 1; }));
    // Chat: the action opens the keyboard, "done" sends.
    guest.press(PadKey::Menu);
    guest.press(PadKey::Left, 2);  // round the edge: Leave, Chat
    guest.press(PadKey::Confirm);
    CHECK(guest.ui.keyboardShown());
    CHECK(guest.type("Hi all"));
    CHECK(guest.pressKey("Done"));
    CHECK(!guest.ui.keyboardShown());
    CHECK(t.until([&] { return chatHas(host.ui, "Hi all"); }));
    // B closes the keyboard without sending.
    guest.press(PadKey::Confirm);
    CHECK(guest.type("no"));
    guest.press(PadKey::Back);
    CHECK(!guest.ui.keyboardShown());
    t.spin(300);
    CHECK(!chatHas(host.ui, "no"));
    CHECK_TEXT(guest.screen(), "lobby");

    // --- the match ---
    host.press(PadKey::Menu);
    host.press(PadKey::Confirm);  // Start
    CHECK(t.until([&] { return host.screen() == "round" && guest.screen() == "round"; }));
    CHECK(host.ui.padsPlay() && guest.ui.padsPlay());
    // Start: the menu. The pads do not steer while it is open.
    guest.press(PadKey::Menu);
    CHECK(guest.ui.matchMenuShown());
    CHECK(!guest.ui.padsPlay());
    guest.press(PadKey::Back);
    CHECK(!guest.ui.matchMenuShown() && guest.ui.padsPlay());
    guest.press(PadKey::Menu);
    guest.press(PadKey::Confirm);  // Resume
    CHECK(!guest.ui.matchMenuShown() && guest.ui.padsPlay());
    guest.press(PadKey::Menu);
    guest.press(PadKey::Down);
    guest.press(PadKey::Confirm);  // Chat
    CHECK(guest.ui.keyboardShown() && !guest.ui.padsPlay());
    CHECK(guest.type("gg"));
    guest.press(PadKey::Menu);
    CHECK(!guest.ui.keyboardShown() && guest.ui.padsPlay());
    CHECK(t.until([&] { return chatHas(host.ui, "gg"); }));
    // Back to the lobby, staying on the server.
    guest.press(PadKey::Menu);
    guest.press(PadKey::Down, 2);
    guest.press(PadKey::Confirm);
    CHECK(t.until([&] { return guest.screen() == "lobby"; }));
    CHECK(guest.ui.client().sittingOut());
    CHECK_TEXT(host.screen(), "round");
    // The host leaves the server from the match: its game is over for everybody.
    host.press(PadKey::Menu);
    host.press(PadKey::Up);  // round the edge to the last item
    host.press(PadKey::Confirm);
    CHECK_TEXT(host.screen(), "host");
    CHECK(t.until([&] { return guest.screen() == "error"; }));
    guest.press(PadKey::Confirm);  // Ok
    CHECK_TEXT(guest.screen(), "join");
    guest.press(PadKey::Up, 2);  // from the address round the top to Back
    guest.press(PadKey::Confirm);
    CHECK_TEXT(guest.screen(), "closed");
    host.press(PadKey::Down);  // Back, below "Start the server"
    host.press(PadKey::Confirm);
    CHECK_TEXT(host.screen(), "closed");
}

// A round played to its end, the result confirmed, then leaving from the lobby and
// cancelling a connection that does not come about.
void testResultAndLeaveByPad() {
    Table t;
    Station& host = t.host;
    host.ui.hostNow(freePort(), false);
    CHECK(t.until([&] { return host.screen() == "lobby"; }));
    host.press(PadKey::Confirm);  // Start, against the computer
    CHECK(t.until([&] { return host.screen() == "round"; }));
    CHECK(t.until([&] { return host.screen() == "result"; }, 400000));
    const std::uint32_t round = host.ui.client().roundId();
    host.press(PadKey::Confirm);  // continue
    CHECK(t.until([&] { return host.screen() != "result" || host.ui.client().roundId() != round; }, 3000));
    // Out of the match by its menu, then out of the lobby by its Leave action.
    if (host.screen() != "lobby") {
        CHECK(t.until([&] { return host.screen() == "round" || host.screen() == "result"; }));
        host.press(PadKey::Menu);
        host.press(PadKey::Down, 2);
        host.press(PadKey::Confirm);
        CHECK(t.until([&] { return host.screen() == "lobby"; }));
    }
    host.press(PadKey::Left);  // round the edge to Leave
    host.press(PadKey::Confirm);
    CHECK_TEXT(host.screen(), "host");
    host.ui.openJoin();

    // Nobody answers at this address: the connection can be given up with B.
    Station& guest = t.guest;
    guest.cfg.netAddress = "127.0.0.1:" + std::to_string(freePort());
    guest.ui.openJoin();
    guest.press(PadKey::Down, 2);
    guest.press(PadKey::Confirm);
    const std::string after = guest.screen();
    CHECK(after == "connecting" || after == "error");
    if (after == "connecting") guest.press(PadKey::Back);
    else guest.press(PadKey::Confirm);
    CHECK_TEXT(guest.screen(), "join");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"pad keys", testPadKeys},
        {"on-screen keyboard", testScreenKeyboard},
        {"network screens by pad", testScreensByPad},
        {"result and leaving by pad", testResultAndLeaveByPad},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
        std::fflush(stdout);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
