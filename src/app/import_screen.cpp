#include "app/import_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "rendering/renderer.hpp"
#include "rendering/sprites.hpp"
#include "resources/asset_import.hpp"

namespace ab {

namespace fs = std::filesystem;

namespace {

// The note left in the folder, so that it is recognised when looked at from a computer.
const char* const kReadme = "PUT-THE-GAME-HERE.txt";

struct Line {
    std::string text;
    float r = 0.85f, g = 0.85f, b = 0.85f;
};
const Line kGap{""};
Line yellow(std::string s) { return {std::move(s), 1.0f, 0.85f, 0.2f}; }
Line white(std::string s) { return {std::move(s), 1.0f, 1.0f, 1.0f}; }
Line grey(std::string s) { return {std::move(s), 0.6f, 0.6f, 0.65f}; }
Line red(std::string s) { return {std::move(s), 1.0f, 0.45f, 0.4f}; }

class Screens {
public:
    Screens(SDL_Window* window, const ImportScreens& setup) : window_(window), setup_(setup) { font_.load(setup.fontDir, 0); }

    bool closed() const { return closed_; }

    // A long text (a path) cut into pieces that fit, preferably after a '/'.
    std::vector<std::string> wrap(const std::string& s, float width) const {
        std::vector<std::string> out;
        std::string rest = s;
        while (renderer_.textWidth(font_, rest) > width && rest.size() > 1) {
            std::size_t cut = rest.size() - 1;
            while (cut > 1 && renderer_.textWidth(font_, rest.substr(0, cut)) > width) --cut;
            const std::size_t fit = cut;
            while (cut > 1 && rest[cut - 1] != '/' && rest[cut - 1] != '\\' && rest[cut - 1] != ' ') --cut;
            if (cut <= 1) cut = fit;
            out.push_back(rest.substr(0, cut));
            rest.erase(0, cut);
        }
        out.push_back(rest);
        return out;
    }

    void addPath(std::vector<Line>& lines, const std::string& path) const {
        for (const std::string& piece : wrap(path, 580.0f)) lines.push_back(white(piece));
    }

    // Handles the window's events. True if a key, a button or the screen was pressed.
    bool pump() {
        bool pressed = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) closed_ = true;
            if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_FINGER_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                pressed = true;
        }
        return pressed;
    }

    // One picture: the lines centred as a block, and, with `bar` >= 0, a progress bar under them.
    void draw(const std::vector<Line>& lines, float bar, const std::string& barText, const std::vector<Line>& below) {
        SDL_GetWindowSizeInPixels(window_, &w_, &h_);
        const float lineH = static_cast<float>(std::max(12, font_.font().height + 4));
        const float barH = bar >= 0.0f ? 64.0f : 0.0f;
        const float total = lineH * static_cast<float>(lines.size() + below.size()) + barH;
        float y = std::max(10.0f, (480.0f - total) / 2.0f);
        renderer_.begin(w_, h_);
        renderer_.quad(0, 0, 640, 480, 0.04f, 0.05f, 0.11f);
        renderer_.quad(0, 0, 640, 4, 1.0f, 0.75f, 0.1f);
        renderer_.quad(0, 476, 640, 4, 1.0f, 0.75f, 0.1f);
        auto put = [&](const std::vector<Line>& block) {
            for (const Line& l : block) {
                if (!l.text.empty()) renderer_.text(font_, l.text, (640.0f - renderer_.textWidth(font_, l.text)) / 2.0f, y, l.r, l.g, l.b);
                y += lineH;
            }
        };
        put(lines);
        if (bar >= 0.0f) {
            const float x = 60.0f, bw = 520.0f, bh = 26.0f, by = y + 14.0f;
            renderer_.quad(x - 3, by - 3, bw + 6, bh + 6, 0.75f, 0.75f, 0.8f);
            renderer_.quad(x - 1, by - 1, bw + 2, bh + 2, 0.02f, 0.02f, 0.05f);
            renderer_.quad(x, by, bw * std::clamp(bar, 0.0f, 1.0f), bh, 0.95f, 0.6f, 0.1f);
            renderer_.quad(x, by, bw * std::clamp(bar, 0.0f, 1.0f), bh / 3.0f, 1.0f, 0.8f, 0.3f);
            renderer_.text(font_, barText, (640.0f - renderer_.textWidth(font_, barText)) / 2.0f,
                           by + (bh - static_cast<float>(font_.font().height)) / 2.0f, 1.0f, 1.0f, 1.0f);
            y += barH;
        }
        put(below);
        renderer_.end();
    }

    void show(const std::string& name) {
        if (setup_.picture && shown_ != name) {
            shown_ = name;
            setup_.picture(name, w_, h_);
        }
        SDL_GL_SwapWindow(window_);
    }

    // A text that stays until a key is pressed (unattended: for a few pictures).
    void message(std::vector<Line> lines, const std::string& name) {
        lines.push_back(kGap);
        lines.push_back(grey("Press a key or touch the screen to go on"));
        pump();  // what was pressed before does not count
        for (int frame = 0; !closed_; ++frame) {
            const bool pressed = pump();
            draw(lines, -1.0f, "", {});
            show(name);
            if ((pressed && frame > 10) || (setup_.automated && frame >= 3)) break;
        }
    }

    // The conversion, running beside the pictures.
    ImportReport convert(const std::string& source) {
        struct Shared {
            std::mutex lock;
            ImportProgress progress;
            ImportReport report;
        } shared;
        std::atomic<bool> stop{false}, finished{false};
        std::thread worker([&]() {
            ImportReport r = importForRelease(source, setup_.converted, setup_.release, [&](const ImportProgress& p) {
                const std::lock_guard<std::mutex> guard(shared.lock);
                shared.progress = p;
                return !stop.load();
            });
            {
                const std::lock_guard<std::mutex> guard(shared.lock);
                shared.report = std::move(r);
            }
            finished = true;
        });
        SDL_DisableScreenSaver();  // a phone must not go to sleep half way
        const Uint64 start = SDL_GetTicks();
        std::vector<Line> head{yellow("CONVERTING THE ORIGINAL GAME DATA"), kGap, grey("From")};
        addPath(head, source);
        head.push_back(kGap);
        while (!finished) {
            pump();
            if (closed_) stop = true;
            ImportProgress p;
            {
                const std::lock_guard<std::mutex> guard(shared.lock);
                p = shared.progress;
            }
            std::vector<Line> lines = head;
            if (p.total == 0)
                lines.push_back(white("Looking at the files..."));
            else
                lines.push_back(white(p.sounds ? "Step 2 of 2: converting the sounds and the music" : "Step 1 of 2: copying graphics, levels and schemes"));
            const float part = p.total > 0 ? static_cast<float>(p.done) / static_cast<float>(p.total) : 0.0f;
            const int seconds = static_cast<int>((SDL_GetTicks() - start) / 1000);
            char buf[160];
            std::vector<Line> below;
            std::snprintf(buf, sizeof buf, "File %d of %d: %s", std::min(p.done + 1, std::max(1, p.total)), p.total, p.item.c_str());
            below.push_back(p.total > 0 ? Line{buf} : kGap);
            // A guess of the time left once there is something to go by.
            if (part > 0.05f && seconds >= 3) {
                const int left = static_cast<int>(static_cast<float>(seconds) * (1.0f - part) / part) + 1;
                std::snprintf(buf, sizeof buf, "Time: %d:%02d, about %d:%02d left", seconds / 60, seconds % 60, left / 60, left % 60);
            } else {
                std::snprintf(buf, sizeof buf, "Time: %d:%02d", seconds / 60, seconds % 60);
            }
            below.push_back(Line{buf});
            below.push_back(kGap);
            below.push_back(grey("This is done once for each release of the game."));
            below.push_back(grey("Please keep the game open until it is finished."));
            std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(part * 100.0f));
            draw(lines, part, buf, below);
            show(part >= 0.5f ? "progress" : "");
        }
        worker.join();
        SDL_EnableScreenSaver();
        return shared.report;
    }

private:
    SDL_Window* window_;
    const ImportScreens& setup_;
    Renderer renderer_;
    SpriteBank font_;
    int w_ = 640, h_ = 480;
    bool closed_ = false;
    std::string shown_;
};

// Something in the folder besides the note this program puts there.
bool holdsOtherFiles(const std::string& folder) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec))
        if (e.path().filename().string() != kReadme) return true;
    return false;
}

}  // namespace

bool runImportScreens(SDL_Window* window, const ImportScreens& setup) {
    std::error_code ec;
    fs::create_directories(setup.folder, ec);
    if (!fs::exists(fs::path(setup.folder) / kReadme, ec)) {
        std::ofstream note(fs::path(setup.folder) / kReadme);
        note << "Atomic Bomberman (modern)\n\n"
                "If you own the original Atomic Bomberman (Interplay, 1997), copy its folder\n"
                "into this one: the installed game or the whole CD, the folder that holds\n"
                "COLOR.PAL and DATA. The next time the game starts it converts the data and\n"
                "plays with the original graphics, sounds and levels.\n\n"
                "Leave the files here: each new release of the game converts them again.\n";
    }
    const std::string source = findOriginalGame(setup.folder);
    const bool noticeShown = fs::exists(setup.noticeFile, ec);
    const bool due = !source.empty() && importIsDue(setup.converted, setup.release);
    const bool unrecognised = source.empty() && holdsOtherFiles(setup.folder);
    if (!due && !unrecognised && noticeShown) return true;

    Screens screens(window, setup);
    if (due) {
        std::fprintf(stderr, "INFO  Converting the original game data from %s\n", source.c_str());
        const ImportReport r = screens.convert(source);
        if (screens.closed()) return false;
        char buf[160];
        std::vector<Line> lines;
        if (r.ok) {
            std::snprintf(buf, sizeof buf, "%d data files and %d sounds, %.0f MB", r.dataFiles, r.sounds, static_cast<double>(r.bytes) / 1.0e6);
            lines = {yellow("THE ORIGINAL GAME DATA IS READY"), kGap, white(buf), kGap,
                     Line{"The game now plays with the original graphics,"}, Line{"sounds and levels."}, kGap,
                     Line{"Leave the files where they are: each new release"}, Line{"of the game converts them again."}};
            std::fprintf(stderr, "INFO  Converted: %s\n", buf);
        } else {
            lines = {red("THE ORIGINAL GAME DATA COULD NOT BE CONVERTED"), kGap};
            for (const std::string& piece : screens.wrap(r.error, 580.0f)) lines.push_back(white(piece));
            lines.push_back(kGap);
            lines.push_back(Line{fs::exists(fs::path(setup.converted), ec) ? "The data converted before stays in use."
                                                                           : "The game goes on with its free graphics and sounds."});
            lines.push_back(Line{"The next start tries again."});
            std::fprintf(stderr, "WARN  Conversion failed: %s\n", r.error.c_str());
        }
        screens.message(lines, "done");
    } else {
        std::vector<Line> lines;
        if (unrecognised) {
            lines = {yellow("NO ORIGINAL GAME FOUND IN THE FOLDER"), kGap, Line{"There are files in"}};
            screens.addPath(lines, setup.folder);
            lines.insert(lines.end(), {Line{"but no copy of Atomic Bomberman: the folder that holds"}, Line{"COLOR.PAL and DATA has to be there, or one folder below."}, kGap,
                                       Line{"The game goes on with what it has."}});
        } else {
            lines = {yellow("ORIGINAL GAME DATA"), kGap, Line{"The game plays with its own free graphics and sounds."}, kGap,
                     Line{"If you own the original Atomic Bomberman (1997),"}, Line{"copy its folder (the one with COLOR.PAL and DATA)"},
                     Line{"into this folder of the device:"}, kGap};
            screens.addPath(lines, setup.folder);
            lines.insert(lines.end(), {kGap, Line{"The next start of the game converts it and plays with"}, Line{"the original graphics, sounds and levels."},
                                       Line{"The folder can be reached from a computer, by USB."}});
        }
        screens.message(lines, "notice");
    }
    if (screens.closed()) return false;
    if (!noticeShown) std::ofstream(setup.noticeFile) << "shown\n";
    return true;
}

}  // namespace ab
