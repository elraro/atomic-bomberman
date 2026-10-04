#include "rendering/sprites.hpp"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <cstdio>

namespace ab {

namespace {

unsigned uploadRgba(int w, int h, const std::vector<std::uint8_t>& rgba) {
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return tex;
}

}  // namespace

SpriteBank::~SpriteBank() {
    for (auto& [key, tex] : textures_) glDeleteTextures(1, &tex);
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
    return true;
}

bool SpriteBank::load(const std::string& gameDir, int level) {
    auto pal = loadPaletteFile(gameDir + "/color.pal");
    if (!pal) {
        std::fprintf(stderr, "WARN  cannot read color.pal under %s\n", gameDir.c_str());
        return false;
    }
    palette_ = std::move(*pal);
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
    addAni(ani + "powers.ani");
    addAni(ani + "shadow.ani");
    addAni(ani + "kick.ani");
    addAni(ani + "extras.ani");
    addAni(ani + "kfont.ani");
    addAni(ani + "hurry.ani");
    addAni(ani + "conveyor.ani");
    for (int n = 1; n <= 4; ++n) {
        addAni(ani + "punbomb" + std::to_string(n) + ".ani");
        addAni(ani + "bwalk" + std::to_string(n) + ".ani");
    }
    for (int n = 1; n <= 17; ++n) addAni(ani + "xplode" + std::to_string(n) + ".ani");

    if (auto pcx = loadPcxFile(gameDir + "/data/res/field" + lv + ".pcx")) {
        std::vector<std::uint8_t> rgba(pcx->indices.size() * 4);
        for (std::size_t i = 0; i < pcx->indices.size(); ++i) {
            const std::uint8_t* c = &pcx->palette[static_cast<std::size_t>(pcx->indices[i]) * 3];
            rgba[i * 4] = c[0];
            rgba[i * 4 + 1] = c[1];
            rgba[i * 4 + 2] = c[2];
            rgba[i * 4 + 3] = 255;
        }
        background_ = uploadRgba(pcx->width, pcx->height, rgba);
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
    const auto it = sequences_.find(name);
    return it == sequences_.end() ? 0 : static_cast<int>(files_[it->second.file].sequences[it->second.seq].steps.size());
}

unsigned SpriteBank::makeTexture(const AniFrame& f, int colour) {
    std::vector<std::uint8_t> rgba(f.pixels.size() * 4);
    const std::vector<std::uint8_t>* remap =
        (colour >= 0 && colour < 10 && !remap_[static_cast<std::size_t>(colour)].empty())
            ? &remap_[static_cast<std::size_t>(colour)]
            : nullptr;
    for (std::size_t i = 0; i < f.pixels.size(); ++i) {
        const std::uint16_t px = f.pixels[i];
        if (f.hasKey && px == f.key) continue;  // transparent
        // As in the original: RGB555 -> palette index, optional player remap, palette colour.
        std::uint8_t idx = palette_.lookup[px & 0x7FFF];
        if (idx == 0) continue;  // index 0 is the transparent colour on screen
        if (remap != nullptr && (*remap)[idx] != 0) idx = (*remap)[idx];
        const std::uint8_t* c = &palette_.rgb6[static_cast<std::size_t>(idx) * 3];
        rgba[i * 4] = static_cast<std::uint8_t>(c[0] * 4 > 255 ? 255 : c[0] * 4);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(c[1] * 4 > 255 ? 255 : c[1] * 4);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(c[2] * 4 > 255 ? 255 : c[2] * 4);
        rgba[i * 4 + 3] = 255;
    }
    return uploadRgba(f.width, f.height, rgba);
}

std::optional<Sprite> SpriteBank::sprite(const std::string& sequence, int index, int colour) {
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
    if (tex == textures_.end()) tex = textures_.emplace(key, makeTexture(f, colour)).first;
    return Sprite{tex->second, f.width, f.height, f.hotX, f.hotY, step.dx, step.dy};
}

}  // namespace ab
