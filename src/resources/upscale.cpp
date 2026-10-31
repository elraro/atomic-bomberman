#include "resources/upscale.hpp"

#include <algorithm>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#include "resources/ani_file.hpp"
#include "resources/png.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ab {

namespace {

std::uint64_t fnv(std::uint64_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t fnvFile(std::uint64_t h, const fs::path& path) {
    const auto bytes = readFileBytes(path.string());
    return bytes ? fnv(h, bytes->data(), bytes->size()) : h;
}

std::string hex(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Part name -> "hash model scale".
using Manifest = std::map<std::string, std::string>;

Manifest loadManifest(const fs::path& file) {
    Manifest m;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab != std::string::npos) m[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return m;
}

// Forces a finished file onto the disk; without it a crash of the machine can leave the file
// with the right size and zeros in it.
void syncFile(const fs::path& path) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FlushFileBuffers(h);
    CloseHandle(h);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
#endif
}

void syncFolder(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) syncFile(e.path());
}

// The new manifest is written beside the old one, forced onto the disk, and only then put in
// its place, so a crash leaves either the old or the new file.
bool saveManifest(const fs::path& file, const Manifest& m) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        for (const auto& [name, value] : m) out << name << '\t' << value << '\n';
        out.flush();
        if (!out) return false;
    }
    syncFile(tmp);
#ifdef _WIN32
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
#endif
}

std::string shellQuote(const fs::path& p) { return "\"" + p.string() + "\""; }

// Runs the upscaling program on a file or a folder; its messages go to `log`.
bool runEsrgan(const UpscaleOptions& o, const fs::path& in, const fs::path& out, const fs::path& log) {
    std::string cmd = shellQuote(o.esrganExe) + " -i " + shellQuote(in) + " -o " + shellQuote(out) + " -n " + o.model + " -s " + std::to_string(o.scale);
    if (!o.modelsDir.empty()) cmd += " -m " + shellQuote(o.modelsDir);
    if (o.gpu >= 0) cmd += " -g " + std::to_string(o.gpu);
    if (o.tile > 0) cmd += " -t " + std::to_string(o.tile);
    cmd += " > " + shellQuote(log) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe drops the outer quotes
#endif
    return std::system(cmd.c_str()) == 0;
}

std::string tail(const fs::path& log) {
    std::ifstream in(log);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (all.size() > 400) all = all.substr(all.size() - 400);
    return all;
}

// Moves a finished folder or file into place, replacing what was there.
bool replace(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::is_directory(from, ec)) syncFolder(from); else syncFile(from);
    fs::create_directories(to.parent_path(), ec);
    fs::remove_all(to, ec);
    ec.clear();
    fs::rename(from, to, ec);
    return !ec;
}

struct SpriteEntry {
    std::string name;  // file name without ".png": "12" or "12_c3"
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// The art has dark dither pixels along the edge of some sprites (the bombs); enlarged they turn
// into a ring of black dots. They are replaced by the mean of their lighter neighbours.
void cleanEdges(std::vector<std::uint8_t>& rgba, int width, int height) {
    auto at = [&](int x, int y) -> const std::uint8_t* { return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4]; };
    auto opaque = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && at(x, y)[3] > 0; };
    auto dark = [&](int x, int y) { const std::uint8_t* p = at(x, y); return p[0] + p[1] + p[2] < 90; };
    struct Fix { int x, y; std::uint8_t rgb[3]; };
    std::vector<Fix> fixes;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (!opaque(x, y) || !dark(x, y)) continue;
            const bool edge = (x > 0 && !opaque(x - 1, y)) || (x + 1 < width && !opaque(x + 1, y)) || (y > 0 && !opaque(x, y - 1)) || (y + 1 < height && !opaque(x, y + 1));
            if (!edge) continue;
            int sum[3] = {0, 0, 0}, n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx || dy) && opaque(x + dx, y + dy) && !dark(x + dx, y + dy)) {
                        const std::uint8_t* q = at(x + dx, y + dy);
                        for (int c = 0; c < 3; ++c) sum[c] += q[c];
                        ++n;
                    }
                }
            if (n > 0) fixes.push_back({x, y, {static_cast<std::uint8_t>(sum[0] / n), static_cast<std::uint8_t>(sum[1] / n), static_cast<std::uint8_t>(sum[2] / n)}});
        }
    for (const Fix& f : fixes) {
        std::uint8_t* p = &rgba[(static_cast<std::size_t>(f.y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(f.x)) * 4];
        p[0] = f.rgb[0];
        p[1] = f.rgb[1];
        p[2] = f.rgb[2];
    }
}

// The pictures to enlarge for one .ani file: every frame plain, and in each player colour
// where that looks different.
std::vector<SpriteEntry> spriteEntries(const AniFile& file, const GamePalette& palette, const std::array<std::vector<std::uint8_t>, 10>& remap, bool clean) {
    std::vector<SpriteEntry> out;
    for (std::size_t i = 0; i < file.frames.size(); ++i) {
        const AniFrame& f = file.frames[i];
        if (f.width < 1 || f.height < 1 || f.pixels.size() != static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height)) continue;
        std::vector<std::uint8_t> plain = aniFrameRgba(f, palette, nullptr);
        if (clean) cleanEdges(plain, f.width, f.height);
        std::vector<SpriteEntry> variants;
        for (int c = 0; c < 10; ++c) {
            if (remap[static_cast<std::size_t>(c)].empty()) continue;
            std::vector<std::uint8_t> coloured = aniFrameRgba(f, palette, &remap[static_cast<std::size_t>(c)]);
            if (clean) cleanEdges(coloured, f.width, f.height);
            if (coloured == plain) continue;  // the game then takes the plain frame
            variants.push_back({std::to_string(i) + "_c" + std::to_string(c), f.width, f.height, std::move(coloured)});
        }
        out.push_back({std::to_string(i), f.width, f.height, std::move(plain)});
        for (auto& v : variants) out.push_back(std::move(v));
    }
    return out;
}

bool pngHasSize(const fs::path& file, int width, int height) {
    const auto img = loadPngFile(file.string());
    return img && img->width == width && img->height == height;
}

struct Part {
    std::string name;   // "ani/walk", "res/mainmenu"
    bool sprites;       // an .ani file; else a .pcx picture
    fs::path source;
};

// Is what a part's target holds complete and of the right size? (To take over the pictures of an
// earlier run whose list was lost; the size of every picture is checked, not the model.)
bool adoptable(const Part& part, const fs::path& target, int scale, const GamePalette& palette, const std::array<std::vector<std::uint8_t>, 10>& remap, bool clean) {
    std::error_code ec;
    if (!part.sprites) {
        const auto pcx = loadPcxFile(part.source.string());
        return pcx && pngHasSize(target, pcx->width * scale, pcx->height * scale);
    }
    const auto file = loadAniFile(part.source.string());
    if (!file || !fs::is_directory(target, ec)) return false;
    const std::vector<SpriteEntry> entries = spriteEntries(*file, palette, remap, clean);
    std::size_t present = 0;
    for (const auto& e : fs::directory_iterator(target, ec)) present += e.is_regular_file(ec) ? 1 : 0;
    if (present != entries.size()) return false;
    for (const SpriteEntry& e : entries)
        if (!pngHasSize(target / (e.name + ".png"), e.width * scale, e.height * scale)) return false;
    return true;
}

// A picture made by the program that is noise, empty or the wrong size (which happens on a
// graphics chip that computes badly) is not an enlargement of its source: shrunk back, it
// differs far more from the source than a real enlargement does.
bool plausible(const PngImage& big, const SpriteEntry& src, int scale) {
    if (big.width != src.width * scale || big.height != src.height * scale) return false;
    const std::size_t w = static_cast<std::size_t>(src.width);
    const std::size_t h = static_cast<std::size_t>(src.height);
    const std::size_t s = static_cast<std::size_t>(scale);
    const std::size_t bw = static_cast<std::size_t>(big.width);
    double colourError = 0, alphaError = 0;
    std::size_t counted = 0;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            double sumA = 0, sumC[3] = {0, 0, 0};
            for (std::size_t dy = 0; dy < s; ++dy)
                for (std::size_t dx = 0; dx < s; ++dx) {
                    const std::uint8_t* q = &big.rgba[((y * s + dy) * bw + (x * s + dx)) * 4];
                    sumA += q[3];
                    for (int c = 0; c < 3; ++c) sumC[c] += static_cast<double>(q[c]) * q[3];
                }
            const double n = static_cast<double>(s * s);
            const double a = sumA / n;
            const std::uint8_t* o = &src.rgba[(y * w + x) * 4];
            if (o[3] == 0 && a < 8) continue;  // empty in both
            ++counted;
            alphaError += std::abs(a - o[3]);
            if (o[3] > 0 && sumA > 0)
                for (int c = 0; c < 3; ++c) colourError += std::abs(sumC[c] / sumA - o[c]) / 3.0 * (o[3] / 255.0);
        }
    }
    if (counted == 0) return true;
    return alphaError / static_cast<double>(counted) < 60.0 && colourError / static_cast<double>(counted) < 50.0;
}

// Enlarges a picture without the model (Catmull-Rom on premultiplied colours): soft, but never
// wrong. For the pictures the model cannot do.
PngImage plainEnlarge(const SpriteEntry& e, int scale) {
    PngImage big;
    big.width = e.width * scale;
    big.height = e.height * scale;
    big.rgba.assign(static_cast<std::size_t>(big.width) * static_cast<std::size_t>(big.height) * 4, 0);
    auto cubic = [](double t) {
        t = std::fabs(t);
        if (t < 1) return 1.5 * t * t * t - 2.5 * t * t + 1;
        if (t < 2) return -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2;
        return 0.0;
    };
    for (int Y = 0; Y < big.height; ++Y) {
        const double fy = (Y + 0.5) / scale - 0.5;
        const int y0 = static_cast<int>(std::floor(fy));
        for (int X = 0; X < big.width; ++X) {
            const double fx = (X + 0.5) / scale - 0.5;
            const int x0 = static_cast<int>(std::floor(fx));
            double sum[4] = {0, 0, 0, 0};
            for (int dy = -1; dy <= 2; ++dy)
                for (int dx = -1; dx <= 2; ++dx) {
                    const int x = std::clamp(x0 + dx, 0, e.width - 1), y = std::clamp(y0 + dy, 0, e.height - 1);
                    const std::uint8_t* q = &e.rgba[(static_cast<std::size_t>(y) * e.width + x) * 4];
                    const double wt = cubic(fx - (x0 + dx)) * cubic(fy - (y0 + dy));
                    const double a = q[3] / 255.0;
                    sum[0] += q[0] * a * wt;
                    sum[1] += q[1] * a * wt;
                    sum[2] += q[2] * a * wt;
                    sum[3] += a * wt;
                }
            const double alpha = std::clamp(sum[3], 0.0, 1.0);
            std::uint8_t* o = &big.rgba[(static_cast<std::size_t>(Y) * big.width + X) * 4];
            if (alpha > 0.002)
                for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>(std::clamp(sum[c] / sum[3], 0.0, 255.0) + 0.5);
            o[3] = static_cast<std::uint8_t>(alpha * 255.0 + 0.5);
        }
    }
    return big;
}

// The picture with `pad` pixels of its own edge repeated all around it: a very small picture
// makes the model give noise or nothing, a bigger one does not.
std::vector<std::uint8_t> padded(const SpriteEntry& e, int pad) {
    const int w = e.width + 2 * pad, h = e.height + 2 * pad;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::copy_n(&e.rgba[(static_cast<std::size_t>(std::clamp(y - pad, 0, e.height - 1)) * e.width + static_cast<std::size_t>(std::clamp(x - pad, 0, e.width - 1))) * 4], 4,
                        &out[(static_cast<std::size_t>(y) * w + x) * 4]);
    return out;
}

// Makes sure every item has a plausible picture "<dir>/<name>.png". The missing and the bad ones
// are made again: as they are, then with an edge around them (the model cannot do very small
// pictures), then, as the last way out, without the model. `repaired` counts the pictures made
// again, `plain` those that had to be enlarged without the model.
bool fixItems(const UpscaleOptions& o, const std::vector<SpriteEntry>& items, const fs::path& dir, const fs::path& work, int& repaired, int& plain, std::string& why) {
    std::error_code ec;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<const SpriteEntry*> bad;
        for (const SpriteEntry& e : items) {
            const auto img = loadPngFile((dir / (e.name + ".png")).string());
            if (!img || !plausible(*img, e, o.scale)) bad.push_back(&e);
        }
        if (bad.empty()) return true;
        if (attempt == 3) {
            for (const SpriteEntry* e : bad) {
                const PngImage big = plainEnlarge(*e, o.scale);
                fs::create_directories(dir, ec);
                if (!writePngFile((dir / (e->name + ".png")).string(), big.width, big.height, big.rgba.data(), true)) {
                    why = "cannot write " + (dir / (e->name + ".png")).string();
                    return false;
                }
                syncFile(dir / (e->name + ".png"));
                ++plain;
                ++repaired;
            }
            return true;
        }
        const int pad = attempt == 0 ? 0 : (attempt == 1 ? 8 : 24);
        fs::remove_all(work / "fix", ec);
        fs::create_directories(work / "fix" / "in", ec);
        fs::create_directories(work / "fix" / "out", ec);
        for (const SpriteEntry* e : bad) {
            if (pad == 0) writePngFile((work / "fix" / "in" / (e->name + ".png")).string(), e->width, e->height, e->rgba.data(), true);
            else writePngFile((work / "fix" / "in" / (e->name + ".png")).string(), e->width + 2 * pad, e->height + 2 * pad, padded(*e, pad).data(), true);
        }
        if (!runEsrgan(o, work / "fix" / "in", work / "fix" / "out", work / "esrgan.log")) {
            why = "the upscaling program failed: " + tail(work / "esrgan.log");
            return false;
        }
        for (const SpriteEntry* e : bad) {
            const fs::path made = work / "fix" / "out" / (e->name + ".png");
            if (!fs::exists(made, ec)) continue;
            fs::create_directories(dir, ec);
            const fs::path target = dir / (e->name + ".png");
            if (pad == 0) {
                syncFile(made);
                fs::remove(target, ec);
                fs::rename(made, target, ec);
            } else {  // cut the edge off again
                const auto img = loadPngFile(made.string());
                if (!img || img->width != (e->width + 2 * pad) * o.scale || img->height != (e->height + 2 * pad) * o.scale) continue;
                PngImage cut;
                cut.width = e->width * o.scale;
                cut.height = e->height * o.scale;
                cut.rgba.resize(static_cast<std::size_t>(cut.width) * static_cast<std::size_t>(cut.height) * 4);
                for (int y = 0; y < cut.height; ++y)
                    std::copy_n(&img->rgba[(static_cast<std::size_t>(y + pad * o.scale) * img->width + static_cast<std::size_t>(pad * o.scale)) * 4], static_cast<std::size_t>(cut.width) * 4,
                                &cut.rgba[static_cast<std::size_t>(y) * cut.width * 4]);
                fs::remove(target, ec);
                if (!writePngFile(target.string(), cut.width, cut.height, cut.rgba.data(), true)) continue;
                syncFile(target);
            }
            ++repaired;
        }
    }
    return false;
}

// The picture of a .pcx part as one item with the given name.
std::optional<SpriteEntry> pictureItem(const Part& part, const std::string& name) {
    const auto pcx = loadPcxFile(part.source.string());
    if (!pcx || pcx->width < 1 || pcx->height < 1) return std::nullopt;
    SpriteEntry e;
    e.name = name;
    e.width = pcx->width;
    e.height = pcx->height;
    e.rgba.resize(pcx->indices.size() * 4);
    for (std::size_t i = 0; i < pcx->indices.size(); ++i) {
        const std::uint8_t* c = &pcx->palette[static_cast<std::size_t>(pcx->indices[i]) * 3];
        e.rgba[i * 4] = c[0];
        e.rgba[i * 4 + 1] = c[1];
        e.rgba[i * 4 + 2] = c[2];
        e.rgba[i * 4 + 3] = 255;
    }
    return e;
}

std::vector<Part> listParts(const UpscaleOptions& o) {
    std::vector<Part> parts;
    auto scan = [&](const fs::path& dir, const char* extension, const char* group, bool sprites) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && lower(e.path().extension().string()) == extension) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) parts.push_back({std::string(group) + "/" + lower(f.stem().string()), sprites, f});
    };
    const fs::path assets(o.assetsDir);
    if (o.sprites) scan(assets / "data" / "ani", ".ani", "ani", true);
    if (o.pictures) scan(assets / "data" / "res", ".pcx", "res", false);
    if (!o.only.empty()) {
        std::vector<Part> chosen;
        for (const Part& p : parts)
            for (const std::string& name : o.only) {
                const std::string n = lower(name);
                if (n == p.name || "ani/" + n == p.name || "res/" + n == p.name) {
                    chosen.push_back(p);
                    break;
                }
            }
        parts = std::move(chosen);
    }
    return parts;
}

}  // namespace

std::string hiresDirFor(const std::string& assetsDir) {
    fs::path p(assetsDir);
    if (!p.empty() && !p.has_filename()) p = p.parent_path();  // a trailing slash
    return (p.parent_path() / "hires").string();
}

UpscaleReport upscaleAssets(const UpscaleOptions& opt, const std::function<bool(const UpscaleProgress&)>& progress) {
    UpscaleReport report;
    UpscaleOptions o = opt;
    const fs::path assets(o.assetsDir);
    std::error_code ec;
    if (!fs::exists(assets / "color.pal", ec) || !fs::is_directory(assets / "data", ec)) {
        report.error = "not an imported asset folder (color.pal and data are required): " + o.assetsDir;
        return report;
    }
    if (o.esrganExe.empty() || !fs::exists(o.esrganExe, ec)) {
        report.error = "the upscaling program was not found: " + (o.esrganExe.empty() ? std::string("(not given)") : o.esrganExe) +
                       " (realesrgan-ncnn-vulkan; name it with --esrgan PATH or ATOMIC_ESRGAN)";
        return report;
    }
    if (o.modelsDir.empty()) o.modelsDir = (fs::path(o.esrganExe).parent_path() / "models").string();
    if (!fs::exists(fs::path(o.modelsDir) / (o.model + ".param"), ec)) {
        report.error = "model not found: " + (fs::path(o.modelsDir) / (o.model + ".param")).string() + " (the .param and .bin files of the model go into the models folder)";
        return report;
    }
    const auto palette = loadPaletteFile((assets / "color.pal").string());
    if (!palette) {
        report.error = "cannot read color.pal";
        return report;
    }
    std::array<std::vector<std::uint8_t>, 10> remap;
    std::uint64_t base = 1469598103934665603ull;
    base = fnvFile(base, assets / "color.pal");
    for (int i = 0; i < 10; ++i) {
        const fs::path rmp = assets / (std::to_string(i) + ".rmp");
        if (auto r = readFileBytes(rmp.string()); r && r->size() >= 256) remap[static_cast<std::size_t>(i)].assign(r->begin(), r->begin() + 256);
        base = fnvFile(base, rmp);
    }
    const std::string how = o.model + " x" + std::to_string(o.scale) + (o.cleanEdges ? " clean1" : "");
    base = fnv(base, reinterpret_cast<const std::uint8_t*>(how.data()), how.size());

    const fs::path hires = o.hiresDir.empty() ? fs::path(hiresDirFor(o.assetsDir)) : fs::path(o.hiresDir);
    const fs::path work = hires / ".work";
    fs::create_directories(hires, ec);
    if (ec) {
        report.error = "cannot create " + hires.string();
        return report;
    }
    const fs::path manifestFile = hires / "manifest.txt";
    Manifest manifest = loadManifest(manifestFile);

    const std::vector<Part> parts = listParts(o);
    UpscaleProgress state;
    state.total = static_cast<int>(parts.size());
    for (const Part& part : parts) {
        state.item = part.name;
        if (progress && !progress(state)) {
            report.cancelled = true;
            report.error = "stopped before the end";
            break;
        }
        ++state.done;
        const std::string key = hex(fnvFile(base, part.source)) + " " + how;
        const fs::path target = hires / (part.sprites ? part.name : part.name + ".png");
        const auto found = manifest.find(part.name);
        if (found != manifest.end() && found->second == key && fs::exists(target, ec)) {
            if (o.verify) {
                std::vector<SpriteEntry> items;
                fs::path dir = target;
                if (part.sprites) {
                    const auto file = loadAniFile(part.source.string());
                    if (file) items = spriteEntries(*file, *palette, remap, o.cleanEdges);
                } else if (auto item = pictureItem(part, target.stem().string())) {
                    items.push_back(std::move(*item));
                    dir = target.parent_path();
                }
                std::string why;
                const int before = report.repaired;
                fs::create_directories(work, ec);
                const bool fine = fixItems(o, items, dir, work, report.repaired, report.plain, why);
                fs::remove_all(work, ec);
                if (!fine) {
                    ++report.failed;
                    std::fprintf(stderr, "WARN  %s: %s\n", part.name.c_str(), why.c_str());
                    continue;
                }
                if (report.repaired != before) std::printf("      %d pictures made again\n", report.repaired - before);
            }
            ++report.current;
            continue;
        }

        if (o.adopt && fs::exists(target, ec) && adoptable(part, target, o.scale, *palette, remap, o.cleanEdges)) {
            manifest[part.name] = key;  // made earlier, the list of what was made is lost
            saveManifest(manifestFile, manifest);
            ++report.adopted;
            continue;
        }

        fs::remove_all(work, ec);
        fs::create_directories(work / "in", ec);
        fs::create_directories(work / "out", ec);
        const fs::path log = work / "esrgan.log";
        bool ok = false;
        std::string why;
        if (part.sprites) {
            const auto file = loadAniFile(part.source.string());
            if (!file) {
                why = "cannot read the file";
            } else {
                const std::vector<SpriteEntry> entries = spriteEntries(*file, *palette, remap, o.cleanEdges);
                int written = 0;
                for (const SpriteEntry& e : entries)
                    if (writePngFile((work / "in" / (e.name + ".png")).string(), e.width, e.height, e.rgba.data(), true)) ++written;
                if (written == 0) {
                    ok = true;  // nothing to enlarge
                } else if (!runEsrgan(o, work / "in", work / "out", log)) {
                    why = "the upscaling program failed: " + tail(log);
                } else {
                    std::size_t made = 0;
                    for (const auto& e : fs::directory_iterator(work / "out", ec)) made += e.is_regular_file(ec) ? 1 : 0;
                    ok = made == static_cast<std::size_t>(written);
                    if (!ok) why = "expected " + std::to_string(written) + " pictures, got " + std::to_string(made);
                }
                if (ok && written > 0) {
                    int again = 0;
                    ok = fixItems(o, entries, work / "out", work, again, report.plain, why);
                }
                if (ok) ok = replace(work / "out", target);
                if (!ok && why.empty()) why = "cannot write " + target.string();
            }
        } else {
            const auto item = pictureItem(part, "picture");
            if (!item) {
                why = "cannot read the file";
            } else {
                const fs::path in = work / "in" / "picture.png";
                const fs::path out = work / "out" / "picture.png";
                int again = 0;
                if (!writePngFile(in.string(), item->width, item->height, item->rgba.data(), false)) why = "cannot write into " + work.string();
                else if (!runEsrgan(o, in, out, log)) why = "the upscaling program failed: " + tail(log);
                else if (!fixItems(o, {*item}, work / "out", work, again, report.plain, why)) {}
                else if (!replace(out, target)) why = "cannot write " + target.string();
                else ok = true;
            }
        }
        fs::remove_all(work, ec);
        if (!ok) {
            ++report.failed;
            std::fprintf(stderr, "WARN  %s: %s\n", part.name.c_str(), why.c_str());
            continue;
        }
        manifest[part.name] = key;
        saveManifest(manifestFile, manifest);
        ++report.made;
    }
    if (!report.cancelled && report.failed == 0) report.ok = true;
    else if (!report.cancelled) report.error = std::to_string(report.failed) + " parts could not be made";
    return report;
}

}  // namespace ab
