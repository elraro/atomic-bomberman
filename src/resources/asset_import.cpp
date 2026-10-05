#include "resources/asset_import.hpp"

#include "resources/mve_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace ab {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Entry of `dir` whose name equals `name` ignoring case; empty path if none.
fs::path findEntry(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    const std::string want = lower(name);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == want) return e.path();
    return {};
}

fs::path findPath(fs::path dir, std::initializer_list<const char*> parts) {
    for (const char* p : parts) {
        dir = findEntry(dir, p);
        if (dir.empty()) return {};
    }
    return dir;
}

bool copyFile(const fs::path& from, const fs::path& to, ImportReport& report) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    // Files copied from a CD arrive read-only, and a read-only file cannot be written over:
    // without this a second import copied nothing (and said "1 data files").
    if (fs::exists(to, ec)) {
        fs::permissions(to, fs::perms::owner_write, fs::perm_options::add, ec);
        fs::remove(to, ec);
    }
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    std::error_code ignored;
    fs::permissions(to, fs::perms::owner_write, fs::perm_options::add, ignored);
    ++report.dataFiles;
    report.bytes += static_cast<long long>(fs::file_size(to, ec));
    return true;
}

// Copies every file of `dir` with one of the extensions, lower-casing the names.
void copyByExtension(const fs::path& dir, const fs::path& to, std::initializer_list<const char*> extensions,
                     ImportReport& report) {
    if (dir.empty()) return;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = lower(e.path().extension().string());
        if (std::find_if(extensions.begin(), extensions.end(), [&](const char* x) { return ext == x; }) == extensions.end())
            continue;
        copyFile(e.path(), to / lower(e.path().filename().string()), report);
    }
}

void put32(std::vector<char>& v, std::uint32_t x) {
    for (int k = 0; k < 4; ++k) v.push_back(static_cast<char>((x >> (8 * k)) & 0xFF));
}
void put16(std::vector<char>& v, std::uint16_t x) {
    v.push_back(static_cast<char>(x & 0xFF));
    v.push_back(static_cast<char>(x >> 8));
}

// .rss is raw PCM: 22050 Hz, stereo, 16-bit signed little-endian. A .wav is the same data behind a header.
bool convertRssToWav(const fs::path& from, const fs::path& to, ImportReport& report) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    const std::vector<char> pcm((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<char> h;
    const auto n = static_cast<std::uint32_t>(pcm.size());
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    put32(h, 36 + n);
    h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, 16);
    put16(h, 1);          // PCM
    put16(h, 2);          // channels
    put32(h, 22050);      // sample rate
    put32(h, 22050 * 4);  // bytes per second
    put16(h, 4);          // block align
    put16(h, 16);         // bits per sample
    h.insert(h.end(), {'d', 'a', 't', 'a'});
    put32(h, n);
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary);
    if (!out) return false;
    out.write(h.data(), static_cast<std::streamsize>(h.size()));
    out.write(pcm.data(), static_cast<std::streamsize>(pcm.size()));
    ++report.sounds;
    report.bytes += static_cast<long long>(h.size() + pcm.size());
    return static_cast<bool>(out);
}

}  // namespace

ImportReport importAssets(const std::string& source, const std::string& destination, bool allSounds) {
    ImportReport report;
    const fs::path src(source);
    const fs::path dst(destination);
    const fs::path palette = findEntry(src, "color.pal");
    const fs::path res = findPath(src, {"data", "res"});
    if (palette.empty() || res.empty() || findEntry(res, "valuelst.res").empty()) {
        report.error = "not an Atomic Bomberman folder (color.pal and data/res/valuelst.res are required): " + source;
        return report;
    }
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        report.error = "cannot create " + destination;
        return report;
    }

    copyByExtension(src, dst, {".pal", ".rmp", ".fon", ".bm"}, report);  // .bm: help pages
    copyByExtension(res, dst / "data" / "res", {".pcx", ".res", ".cam"}, report);  // .cam: campaigns
    copyByExtension(findPath(src, {"data", "schemes"}), dst / "data" / "schemes", {".sch"}, report);
    copyByExtension(findPath(src, {"data", "ani"}), dst / "data" / "ani", {".ani", ".ali"}, report);

    // The intro movie sits at the end of the original's small player program; only the
    // movie is taken, not the program.
    if (const fs::path player = findEntry(findEntry(src, "intro"), "bmintro.exe"); !player.empty()) {
        MveDecoder movie;
        if (movie.open(player.string())) {
            const auto& all = movie.fileBytes();
            std::ofstream out(dst / "intro.mve", std::ios::binary);
            out.write(reinterpret_cast<const char*>(all.data() + movie.movieOffset()),
                      static_cast<std::streamsize>(all.size() - movie.movieOffset()));
            if (out) {
                ++report.dataFiles;
                report.bytes += static_cast<long long>(all.size() - movie.movieOffset());
            }
        }
    }

    // Sounds named in soundlst.res ("id,name" lines; ';' starts a comment).
    const fs::path soundDir = findPath(src, {"data", "sound"});
    std::ifstream list(findEntry(res, "soundlst.res"), std::ios::binary);
    std::string line;
    std::vector<std::string> done;
    while (!soundDir.empty() && std::getline(list, line)) {
        if (auto semi = line.find(';'); semi != std::string::npos) line.erase(semi);
        const auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        std::string name = line.substr(comma + 1);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; }), name.end());
        name = lower(name);
        if (name.empty() || std::find(done.begin(), done.end(), name) != done.end()) continue;
        done.push_back(name);
        const fs::path rss = findEntry(soundDir, name + ".rss");
        if (rss.empty() || !convertRssToWav(rss, dst / "data" / "sound" / (name + ".wav"), report)) {
            ++report.missingSounds;
            report.missing.push_back(name);
        }
    }

    if (allSounds && !soundDir.empty()) {
        std::error_code scan;
        for (const auto& e : fs::directory_iterator(soundDir, scan)) {
            if (!e.is_regular_file(scan) || lower(e.path().extension().string()) != ".rss") continue;
            const std::string name = lower(e.path().stem().string());
            if (std::find(done.begin(), done.end(), name) != done.end()) continue;
            const int before = report.sounds;
            if (convertRssToWav(e.path(), dst / "data" / "sound" / (name + ".wav"), report)) {
                report.sounds = before;  // counted apart
                ++report.extraSounds;
            }
        }
    }

    std::ofstream note(dst / "ABOUT-THESE-FILES.txt");
    note << "These files were produced on this computer from the owner's copy of\n"
            "Atomic Bomberman (c) 1997 Interplay Productions / Hudson Soft, by the\n"
            "modern reimplementation's --import-assets command.\n"
            "They are copyrighted material of their owners. Do not redistribute them.\n";
    report.ok = true;
    return report;
}

}  // namespace ab
