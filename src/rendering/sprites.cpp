#include "rendering/sprites.hpp"

#include <algorithm>
#include <cctype>

#include "free/free_data.hpp"
#include "rendering/gl.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "resources/png.hpp"
#include "resources/bomb_redraw.hpp"
#include "resources/sprite_refine.hpp"
#include "resources/upscale.hpp"

namespace ab {

namespace {

std::size_t gTextureBytes = 0;  // pixels uploaded so far, for the debug window
bool gHiresEnabled = true;
bool gHiresSmoothEdges = true;

// `smooth`: enlarged pictures are drawn smaller than they are, so they are filtered; the
// original's small pictures are drawn as they are, with hard pixels.
unsigned uploadRgba(int w, int h, const std::vector<std::uint8_t>& rgba, bool smooth = false) {
    gTextureBytes += rgba.size();
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return tex;
}

}  // namespace

void SpriteBank::setHiresEnabled(bool on) { gHiresEnabled = on; }

void SpriteBank::setHiresSmoothEdges(bool on) { gHiresSmoothEdges = on; }

unsigned SpriteBank::hiresTexture(const std::string& path, int w, int h, const std::function<void(PngImage&)>& post) const {
    if (hiresDir_.empty() || w < 1 || h < 1) return 0;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    return hiresTextureFrom(loadPngFile(path), path, w, h, post);
}

unsigned SpriteBank::hiresTextureFrom(std::optional<PngImage> img, const std::string& label, int w, int h, const std::function<void(PngImage&)>& post) const {
    if (!img || img->width % w != 0 || img->height % h != 0 || img->width / w != img->height / h) {
        std::fprintf(stderr, "WARN  enlarged picture does not fit, the original is used: %s\n", label.c_str());
        return 0;
    }
    PngImage big = std::move(*img);
    if (post && gHiresSmoothEdges) post(big);
    return uploadRgba(big.width, big.height, big.rgba, true);
}

unsigned SpriteBank::picture(const std::string& name) {
    const auto it = pictures_.find(name);
    if (it != pictures_.end()) return it->second;
    unsigned tex = 0;
    if (free_) {
        if (const auto pic = makeFreePicture(name)) {
            tex = uploadRgba(pic->width, pic->height, pic->rgba);
            pictureSizes_[name] = {pic->width, pic->height};
        }
    } else if (auto pcx = loadPcxFile(gameDir_ + "/data/res/" + name + ".pcx")) {
        tex = hiresTexture(hiresDir_ + "/res/" + name + ".png", pcx->width, pcx->height);
        if (tex == 0) {
            std::vector<std::uint8_t> rgba(pcx->indices.size() * 4);
            for (std::size_t i = 0; i < pcx->indices.size(); ++i) {
                const std::uint8_t* c = &pcx->palette[static_cast<std::size_t>(pcx->indices[i]) * 3];
                rgba[i * 4] = c[0];
                rgba[i * 4 + 1] = c[1];
                rgba[i * 4 + 2] = c[2];
                rgba[i * 4 + 3] = 255;
            }
            tex = uploadRgba(pcx->width, pcx->height, rgba);
        }
        pictureSizes_[name] = {pcx->width, pcx->height};
    }
    pictures_[name] = tex;
    return tex;
}

bool SpriteBank::pictureSize(const std::string& name, int* width, int* height) {
    if (picture(name) == 0) return false;
    const auto it = pictureSizes_.find(name);
    if (it == pictureSizes_.end()) return false;
    *width = it->second.first;
    *height = it->second.second;
    return true;
}

std::vector<std::string> SpriteBank::sequenceNames() const {
    std::vector<std::string> names;
    for (const auto& [name, ref] : sequences_) names.push_back(name);
    for (const auto& [name, frames] : art_.sequences) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

std::size_t SpriteBank::textureBytes() { return gTextureBytes; }

void SpriteBank::ensureTiles(int level) {
    if (free_) return;  // the free set holds every level's tiles
    if (level < 0 || level > 10 || level == level_ || tilesLoaded_[static_cast<std::size_t>(level)]) return;
    tilesLoaded_[static_cast<std::size_t>(level)] = true;
    addAni(gameDir_ + "/data/ani/tiles" + std::to_string(level) + ".ani");
}

SpriteBank::~SpriteBank() {
    for (auto& [name, tex] : pictures_)
        if (tex != 0) glDeleteTextures(1, &tex);
    for (auto& [key, tex] : textures_) glDeleteTextures(1, &tex);
    for (auto& [key, tex] : freeTextures_) glDeleteTextures(1, &tex);
    if (background_ != 0) glDeleteTextures(1, &background_);
    if (font_.texture != 0) glDeleteTextures(1, &font_.texture);
}

bool SpriteBank::addAni(const std::string& path) {
    auto file = loadAniFile(path);
    if (!file) {
        std::fprintf(stderr, "WARN  cannot read %s\n", path.c_str());
        return false;
    }
    const std::size_t fi = files_.size();
    for (std::size_t s = 0; s < file->sequences.size(); ++s) sequences_[file->sequences[s].name] = {fi, s};
    files_.push_back(std::move(*file));
    std::string stem = std::filesystem::path(path).stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    hiresPacks_.push_back(hiresDir_.empty() ? std::nullopt : HiresPack::open(hiresDir_ + "/ani/" + stem + ".hani"));
    fileStems_.push_back(std::move(stem));
    return true;
}

void SpriteBank::addMenuBomb() {
    const auto trig = sequences_.find("bomb trigger green");
    const auto reg = sequences_.find("bomb regular green");
    if (trig == sequences_.end() || reg == sequences_.end() || palette_.lookup.size() < 32768 || palette_.rgb6.size() < 768) return;
    const AniFile& tf = files_[trig->second.file];
    const AniFile& rf = files_[reg->second.file];
    const AniSequence& ts = tf.sequences[trig->second.seq];
    const AniSequence& rs = rf.sequences[reg->second.seq];
    if (ts.steps.empty() || rs.steps.empty()) return;
    const AniFrame& big = tf.frames[static_cast<std::size_t>(ts.steps[0].frame)];  // the largest: "BOOM" can be read
    if (big.pixels.empty() || !big.hasKey) return;
    int widest = 1;
    for (const AniStep& st : rs.steps) widest = std::max(widest, rf.frames[static_cast<std::size_t>(st.frame)].width);

    // The ball (the green of the original's palette) gives the centre the bomb swells and shrinks around;
    // the fuse cap sticks out of it, so the window around that centre is as large as the picture needs.
    auto opaque = [&](int x, int y) { return big.pixels[static_cast<std::size_t>(y * big.width + x)] != big.key; };
    int bx0 = big.width, by0 = big.height, bx1 = -1, by1 = -1;
    for (int y = 0; y < big.height; ++y)
        for (int x = 0; x < big.width; ++x) {
            if (!opaque(x, y)) continue;
            const std::uint8_t idx = palette_.lookup[big.pixels[static_cast<std::size_t>(y * big.width + x)] & 0x7FFF];
            if (idx == 0) continue;
            const std::uint8_t* c = &palette_.rgb6[static_cast<std::size_t>(idx) * 3];
            if (!(c[1] > 1.25f * c[0] && c[1] > 1.5f * c[2])) continue;
            bx0 = std::min(bx0, x);
            by0 = std::min(by0, y);
            bx1 = std::max(bx1, x);
            by1 = std::max(by1, y);
        }
    if (bx1 < bx0) return;
    const int ax = (bx0 + bx1 + 1) / 2, ay = (by0 + by1 + 1) / 2;  // the centre, on a whole pixel
    int half = 1;
    for (int y = 0; y < big.height; ++y)
        for (int x = 0; x < big.width; ++x)
            if (opaque(x, y)) half = std::max(half, std::max(std::max(ax - x, x + 1 - ax), std::max(ay - y, y + 1 - ay)));
    const int window = 2 * half;
    menuBombFile_ = trig->second.file;
    menuBombFrame_ = ts.steps[0].frame;
    menuBombX_ = ax - half;
    menuBombY_ = ay - half;
    menuBombWindow_ = window;

    AniFile out;
    AniSequence seq;
    seq.name = "menu bomb";
    std::map<int, int> made;  // target width -> frame index
    for (const AniStep& st : rs.steps) {
        const int target = rf.frames[static_cast<std::size_t>(st.frame)].width;
        auto it = made.find(target);
        if (it == made.end()) {
            // Always an even number of pixels, the hot spot in the middle: the ball keeps its centre in every step.
            const int size = std::max(2, 2 * static_cast<int>(std::lround(static_cast<float>(window) * static_cast<float>(target) / static_cast<float>(widest) / 2.0f)));
            AniFrame f;
            f.width = f.height = size;
            f.hotX = f.hotY = size / 2;
            f.hasKey = true;
            f.key = big.key;
            f.pixels.assign(static_cast<std::size_t>(size * size), big.key);
            const float step = static_cast<float>(window) / static_cast<float>(size);  // source pixels per new pixel
            // Each new pixel is the mean of the source area it covers; mostly empty areas stay transparent.
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const float x0 = static_cast<float>(x) * step + static_cast<float>(menuBombX_), x1 = x0 + step;
                    const float y0 = static_cast<float>(y) * step + static_cast<float>(menuBombY_), y1 = y0 + step;
                    float sum[3] = {0, 0, 0}, solid = 0, area = 0;
                    for (int sy = static_cast<int>(std::floor(y0)); sy < static_cast<int>(std::ceil(y1)); ++sy)
                        for (int sx = static_cast<int>(std::floor(x0)); sx < static_cast<int>(std::ceil(x1)); ++sx) {
                            const float w = (std::min(x1, static_cast<float>(sx + 1)) - std::max(x0, static_cast<float>(sx))) *
                                            (std::min(y1, static_cast<float>(sy + 1)) - std::max(y0, static_cast<float>(sy)));
                            area += w;
                            if (sx < 0 || sy < 0 || sx >= big.width || sy >= big.height) continue;
                            const std::uint16_t c = big.pixels[static_cast<std::size_t>(sy * big.width + sx)];
                            if (c == big.key) continue;
                            solid += w;
                            sum[0] += w * static_cast<float>((c >> 10) & 31);
                            sum[1] += w * static_cast<float>((c >> 5) & 31);
                            sum[2] += w * static_cast<float>(c & 31);
                        }
                    if (area <= 0 || solid * 2.0f < area) continue;
                    auto c = static_cast<std::uint16_t>((std::lround(sum[0] / solid) << 10) | (std::lround(sum[1] / solid) << 5) | std::lround(sum[2] / solid));
                    if (c == big.key) c ^= 1;  // never equal to the key colour
                    f.pixels[static_cast<std::size_t>(y * size + x)] = c;
                }
            it = made.emplace(target, static_cast<int>(out.frames.size())).first;
            out.frames.push_back(std::move(f));
        }
        seq.steps.push_back({it->second, 0, 0});
    }
    out.sequences.push_back(seq);
    const std::size_t fi = files_.size();
    sequences_[seq.name] = {fi, 0};
    files_.push_back(std::move(out));
    hiresPacks_.push_back(std::nullopt);
    fileStems_.push_back("menubomb");
}

bool SpriteBank::loadFree(int level) {
    free_ = true;
    art_ = makeFreeArt();
    level_ = level;
    if (const auto field = makeFreePicture("field" + std::to_string(level))) background_ = uploadRgba(field->width, field->height, field->rgba);
    const FreeFont ff = makeFreeFont();
    std::vector<std::uint8_t> rgba(ff.alpha.size() * 4, 255);
    for (std::size_t i = 0; i < ff.alpha.size(); ++i) rgba[i * 4 + 3] = ff.alpha[i];
    font_.texture = uploadRgba(ff.atlasWidth, ff.height, rgba);
    font_.height = ff.height;
    font_.spacing = ff.spacing;
    font_.atlasWidth = ff.atlasWidth;
    font_.x = ff.x;
    font_.width = ff.width;
    loaded_ = true;
    std::fprintf(stderr, "INFO  Free graphics set made: frames=%zu sequences=%zu level=%d\n", art_.frames.size(), art_.sequences.size(), level);
    return true;
}

bool SpriteBank::load(const std::string& gameDir, int level) {
    gameDir_ = gameDir;
    if (isFreeAssetDir(gameDir) && !std::ifstream(gameDir + "/color.pal").good()) return loadFree(level);
    auto pal = loadPaletteFile(gameDir + "/color.pal");
    if (!pal) {
        std::fprintf(stderr, "WARN  cannot read color.pal under %s\n", gameDir.c_str());
        return false;
    }
    palette_ = std::move(*pal);
    gameDir_ = gameDir;
    hiresDir_.clear();
    if (gHiresEnabled) {
        const std::string dir = hiresDirFor(gameDir);
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
            hiresDir_ = dir;
            std::fprintf(stderr, "INFO  Enlarged graphics: %s\n", dir.c_str());
        }
    }
    for (int i = 0; i < 10; ++i)
        if (auto r = readFileBytes(gameDir + "/" + std::to_string(i) + ".rmp"); r && r->size() >= 256)
            remap_[static_cast<std::size_t>(i)].assign(r->begin(), r->begin() + 256);

    const std::string ani = gameDir + "/data/ani/";
    const std::string lv = std::to_string(level);
    bool ok = addAni(ani + "tiles" + lv + ".ani");
    ok = addAni(ani + "walk.ani") && ok;
    ok = addAni(ani + "stand.ani") && ok;
    ok = addAni(ani + "bombs.ani") && ok;
    ok = addAni(ani + "mflame.ani") && ok;
    addAni(ani + "xbrick" + lv + ".ani");
    addAni(ani + "triganim.ani");
    addAni(ani + "duds.ani");
    addAni(ani + "powers.ani");
    addAni(ani + "shadow.ani");
    addAni(ani + "kick.ani");
    addAni(ani + "extras.ani");
    addAni(ani + "kfont.ani");
    addAni(ani + "misc.ani");
    addAni(ani + "hurry.ani");
    addAni(ani + "conveyor.ani");
    addAni(ani + "aliens1.ani");
    addMenuBomb();
    for (int n = 1; n <= 4; ++n) {
        addAni(ani + "punbomb" + std::to_string(n) + ".ani");
        addAni(ani + "bwalk" + std::to_string(n) + ".ani");
        addAni(ani + "pup" + std::to_string(n) + ".ani");
    }
    for (int n = 0; n <= 7; ++n) addAni(ani + "corner" + std::to_string(n) + ".ani");
    for (int n = 1; n <= 17; ++n) addAni(ani + "xplode" + std::to_string(n) + ".ani");

    if (auto pcx = loadPcxFile(gameDir + "/data/res/field" + lv + ".pcx")) {
        background_ = hiresTexture(hiresDir_ + "/res/field" + lv + ".png", pcx->width, pcx->height);
        if (background_ == 0) {
            std::vector<std::uint8_t> rgba(pcx->indices.size() * 4);
            for (std::size_t i = 0; i < pcx->indices.size(); ++i) {
                const std::uint8_t* c = &pcx->palette[static_cast<std::size_t>(pcx->indices[i]) * 3];
                rgba[i * 4] = c[0];
                rgba[i * 4 + 1] = c[1];
                rgba[i * 4 + 2] = c[2];
                rgba[i * 4 + 3] = 255;
            }
            background_ = uploadRgba(pcx->width, pcx->height, rgba);
        }
    } else {
        std::fprintf(stderr, "WARN  cannot read field%s.pcx\n", lv.c_str());
    }
    if (auto ff = loadFontFile(gameDir + "/font1.fon")) {
        int total = 0;
        for (const FontGlyph& g : ff->glyphs) total += g.width + 1;
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(total * ff->height) * 4, 0);
        int x = 0;
        for (const FontGlyph& g : ff->glyphs) {
            font_.x.push_back(x);
            font_.width.push_back(g.width);
            for (int yy = 0; yy < ff->height; ++yy)
                for (int xx = 0; xx < g.width; ++xx) {
                    const std::size_t o = static_cast<std::size_t>(yy * total + x + xx) * 4;
                    rgba[o] = rgba[o + 1] = rgba[o + 2] = 255;
                    rgba[o + 3] = g.alpha[static_cast<std::size_t>(yy * g.width + xx)];
                }
            x += g.width + 1;
        }
        font_.texture = uploadRgba(total, ff->height, rgba);
        font_.height = ff->height;
        font_.spacing = ff->spacing;
        font_.atlasWidth = total;
    }
    loaded_ = ok;
    level_ = level;
    std::fprintf(stderr, "INFO  Original graphics %s: files=%zu sequences=%zu level=%d\n", ok ? "loaded" : "incomplete",
                 files_.size(), sequences_.size(), level);
    return ok;
}

int SpriteBank::sequenceLength(const std::string& name) const {
    if (free_) {
        const auto found = art_.sequences.find(name);
        return found == art_.sequences.end() ? 0 : static_cast<int>(found->second.size());
    }
    const auto it = sequences_.find(name);
    return it == sequences_.end() ? 0 : static_cast<int>(files_[it->second.file].sequences[it->second.seq].steps.size());
}

unsigned SpriteBank::makeTexture(const AniFrame& f, int colour) {
    const std::vector<std::uint8_t>* remap =
        (colour >= 0 && colour < 10 && !remap_[static_cast<std::size_t>(colour)].empty())
            ? &remap_[static_cast<std::size_t>(colour)]
            : nullptr;
    return uploadRgba(f.width, f.height, aniFrameRgba(f, palette_, remap));
}

bool SpriteBank::spriteRgba(const std::string& sequence, int index, int colour, int* width, int* height, std::vector<std::uint8_t>* rgba) {
    if (free_) {
        const auto found = art_.sequences.find(sequence);
        if (found == art_.sequences.end() || found->second.empty()) return false;
        const FreeFrame& f = art_.frames[static_cast<std::size_t>(found->second[static_cast<std::size_t>(index) % found->second.size()])];
        *width = f.width, *height = f.height;
        *rgba = colouredFrame(f, colour);
        return true;
    }
    const auto it = sequences_.find(sequence);
    if (it == sequences_.end()) return false;
    const AniSequence& seq = files_[it->second.file].sequences[it->second.seq];
    if (seq.steps.empty()) return false;
    const int frame = seq.steps[static_cast<std::size_t>(index) % seq.steps.size()].frame;
    const auto& frames = files_[it->second.file].frames;
    if (frame < 0 || static_cast<std::size_t>(frame) >= frames.size() || frames[static_cast<std::size_t>(frame)].pixels.empty()) return false;
    const AniFrame& f = frames[static_cast<std::size_t>(frame)];
    const std::vector<std::uint8_t>* remap =
        (colour >= 0 && colour < 10 && !remap_[static_cast<std::size_t>(colour)].empty()) ? &remap_[static_cast<std::size_t>(colour)] : nullptr;
    *width = f.width, *height = f.height;
    *rgba = aniFrameRgba(f, palette_, remap);
    return true;
}

std::optional<Sprite> SpriteBank::sprite(const std::string& sequence, int index, int colour) {
    if (free_) {
        const auto found = art_.sequences.find(sequence);
        if (found == art_.sequences.end() || found->second.empty()) return std::nullopt;
        const int frame = found->second[static_cast<std::size_t>(index) % found->second.size()];
        const FreeFrame& f = art_.frames[static_cast<std::size_t>(frame)];
        const auto key = std::make_pair(frame, colour);
        auto tex = freeTextures_.find(key);
        if (tex == freeTextures_.end()) tex = freeTextures_.emplace(key, uploadRgba(f.width, f.height, colouredFrame(f, colour))).first;
        return Sprite{tex->second, f.width, f.height, f.hotX, f.hotY, 0, 0};
    }
    const auto it = sequences_.find(sequence);
    if (it == sequences_.end()) return std::nullopt;
    const AniFile& file = files_[it->second.file];
    const AniSequence& seq = file.sequences[it->second.seq];
    if (seq.steps.empty()) return std::nullopt;
    const AniStep& step = seq.steps[static_cast<std::size_t>(index) % seq.steps.size()];
    if (step.frame < 0 || static_cast<std::size_t>(step.frame) >= file.frames.size()) return std::nullopt;
    const AniFrame& f = file.frames[static_cast<std::size_t>(step.frame)];
    if (f.pixels.empty()) return std::nullopt;

    const auto key = std::make_tuple(it->second.file, step.frame, colour);
    auto tex = textures_.find(key);
    if (tex == textures_.end()) {
        unsigned texture = 0;
        if (!hiresDir_.empty()) {
            // The enlarged frame in the player's colour if it differs from the plain one, else the plain one.
            // "menu bomb" has no enlarged pictures of its own: it draws the trigger bomb's largest one, made smaller by its size.
            const bool menu = fileStems_[it->second.file] == "menubomb";
            const std::size_t hf = menu ? menuBombFile_ : it->second.file;
            const int hframe = menu ? menuBombFrame_ : step.frame;
            const AniFrame& src = menu ? files_[hf].frames[static_cast<std::size_t>(hframe)] : f;
            const std::string base = hiresDir_ + "/ani/" + fileStems_[hf] + "/" + std::to_string(hframe);
            const std::vector<std::uint8_t> plain = aniFrameRgba(src, palette_, nullptr);  // the mask does not depend on the colour
            const std::vector<std::uint8_t>* table = (colour >= 0 && colour < 10 && !remap_[static_cast<std::size_t>(colour)].empty()) ? &remap_[static_cast<std::size_t>(colour)] : nullptr;
            const std::string& stem = fileStems_[hf];
            const bool bomb = stem == "bombs" || stem == "bombanim" || stem == "bomanim2" || stem == "triganim" || stem == "trigbomb";  // balls with a fuse cap
            const auto post = [&](PngImage& big) {
                refineSilhouette(big, src.width, src.height, plain);
                if (bomb) redrawBomb(big, src, palette_, table);
                if (menu) {  // only the window around the ball's centre, as the frames of "menu bomb" are
                    const int m = big.width / src.width, side = menuBombWindow_ * m;
                    PngImage win;
                    win.width = win.height = side;
                    win.rgba.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4, 0);
                    for (int y = 0; y < side; ++y)
                        for (int x = 0; x < side; ++x) {
                            const int sx = x + menuBombX_ * m, sy = y + menuBombY_ * m;
                            if (sx < 0 || sy < 0 || sx >= big.width || sy >= big.height) continue;
                            std::copy_n(&big.rgba[(static_cast<std::size_t>(sy) * big.width + sx) * 4], 4, &win.rgba[(static_cast<std::size_t>(y) * side + x) * 4]);
                        }
                    big = std::move(win);
                }
            };
            // The container of the file first, then the folder of PNGs that older runs made.
            const auto& pack = hiresPacks_[hf];
            const auto enlarged = [&](int c) -> unsigned {
                if (pack)
                    if (const auto bytes = pack->read(hframe, c))
                        if (const unsigned t = hiresTextureFrom(decodePng(*bytes), stem + ".hani/" + hiresFrameName(hframe, c), src.width, src.height, post)) return t;
                return hiresTexture(base + (c >= 0 ? "_c" + std::to_string(c) : std::string()) + ".png", src.width, src.height, post);
            };
            if (colour >= 0) texture = enlarged(colour);
            if (texture == 0) texture = enlarged(-1);
        }
        if (texture == 0) texture = makeTexture(f, colour);
        tex = textures_.emplace(key, texture).first;
    }
    return Sprite{tex->second, f.width, f.height, f.hotX, f.hotY, step.dx, step.dy};
}

}  // namespace ab
