#include "resources/hires_pack.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>

#include "resources/ani_file.hpp"

namespace fs = std::filesystem;

namespace ab {

namespace {

constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kPlain = 0xFFFFFFFFu;
constexpr std::size_t kHeader = 16;
constexpr std::size_t kEntry = 16;
constexpr std::uint32_t kMaxCount = 1u << 20;

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

}  // namespace

bool writeHiresPack(const std::string& path, int scale, std::vector<HiresPackItem> items) {
    std::sort(items.begin(), items.end(), [](const HiresPackItem& a, const HiresPackItem& b) { return a.frame != b.frame ? a.frame < b.frame : a.colour < b.colour; });
    std::vector<std::uint8_t> head;
    head.reserve(kHeader + items.size() * kEntry);
    head.insert(head.end(), {'H', 'A', 'N', 'I'});
    put32(head, kVersion);
    put32(head, static_cast<std::uint32_t>(scale));
    put32(head, static_cast<std::uint32_t>(items.size()));
    std::uint64_t offset = kHeader + items.size() * kEntry;
    for (const HiresPackItem& it : items) {
        if (it.frame < 0 || it.png.empty() || offset + it.png.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        put32(head, static_cast<std::uint32_t>(it.frame));
        put32(head, it.colour < 0 ? kPlain : static_cast<std::uint32_t>(it.colour));
        put32(head, static_cast<std::uint32_t>(offset));
        put32(head, static_cast<std::uint32_t>(it.png.size()));
        offset += it.png.size();
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    for (const HiresPackItem& it : items) out.write(reinterpret_cast<const char*>(it.png.data()), static_cast<std::streamsize>(it.png.size()));
    out.flush();
    return static_cast<bool>(out);
}

std::optional<HiresPack> HiresPack::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0);
    std::uint8_t head[kHeader];
    if (!in.read(reinterpret_cast<char*>(head), kHeader)) return std::nullopt;
    if (head[0] != 'H' || head[1] != 'A' || head[2] != 'N' || head[3] != 'I' || get32(head + 4) != kVersion) return std::nullopt;
    const std::uint32_t count = get32(head + 12);
    if (count > kMaxCount || kHeader + static_cast<std::uint64_t>(count) * kEntry > fileSize) return std::nullopt;
    std::vector<std::uint8_t> dir(static_cast<std::size_t>(count) * kEntry);
    if (!dir.empty() && !in.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dir.size()))) return std::nullopt;

    HiresPack pack;
    pack.path_ = path;
    pack.scale_ = static_cast<int>(get32(head + 8));
    pack.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = &dir[static_cast<std::size_t>(i) * kEntry];
        const std::uint32_t colour = get32(e + 4), offset = get32(e + 8), size = get32(e + 12);
        if (get32(e) > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || (colour != kPlain && colour > 255) || size == 0 ||
            static_cast<std::uint64_t>(offset) + size > fileSize)
            return std::nullopt;
        pack.entries_.push_back({static_cast<int>(get32(e)), colour == kPlain ? -1 : static_cast<int>(colour), offset, size});
    }
    std::sort(pack.entries_.begin(), pack.entries_.end(), [](const Entry& a, const Entry& b) { return a.frame != b.frame ? a.frame < b.frame : a.colour < b.colour; });
    return pack;
}

const HiresPack::Entry* HiresPack::find(int frame, int colour) const {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), std::make_pair(frame, colour),
                                     [](const Entry& e, const std::pair<int, int>& k) { return e.frame != k.first ? e.frame < k.first : e.colour < k.second; });
    return it != entries_.end() && it->frame == frame && it->colour == colour ? &*it : nullptr;
}

bool HiresPack::has(int frame, int colour) const { return find(frame, colour) != nullptr; }

std::optional<std::vector<std::uint8_t>> HiresPack::read(int frame, int colour) const {
    const Entry* e = find(frame, colour);
    if (!e) return std::nullopt;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(e->offset);
    std::vector<std::uint8_t> bytes(e->size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    return bytes;
}

std::vector<std::pair<int, int>> HiresPack::keys() const {
    std::vector<std::pair<int, int>> k;
    k.reserve(entries_.size());
    for (const Entry& e : entries_) k.emplace_back(e.frame, e.colour);
    return k;
}

std::string hiresFrameName(int frame, int colour) {
    return std::to_string(frame) + (colour >= 0 ? "_c" + std::to_string(colour) : std::string());
}

bool parseHiresFrameName(const std::string& name, int* frame, int* colour) {
    std::size_t i = 0;
    long f = 0;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])) && i < 9) f = f * 10 + (name[i++] - '0');
    if (i == 0) return false;
    long c = -1;
    if (i < name.size()) {
        if (name.compare(i, 2, "_c") != 0 || i + 2 >= name.size() || name.size() - (i + 2) > 3) return false;
        c = 0;
        for (std::size_t k = i + 2; k < name.size(); ++k) {
            if (!std::isdigit(static_cast<unsigned char>(name[k]))) return false;
            c = c * 10 + (name[k] - '0');
        }
    }
    *frame = static_cast<int>(f);
    *colour = static_cast<int>(c);
    return true;
}

bool packHiresFolder(const std::string& dir, int scale, const std::string& packFile) {
    std::error_code ec;
    std::vector<HiresPackItem> items;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".png") continue;
        HiresPackItem it;
        if (!parseHiresFrameName(e.path().stem().string(), &it.frame, &it.colour)) continue;
        auto bytes = readFileBytes(e.path().string());
        if (!bytes || bytes->empty()) return false;
        it.png = std::move(*bytes);
        items.push_back(std::move(it));
    }
    if (ec) return false;
    return writeHiresPack(packFile, scale, std::move(items));
}

bool unpackHiresPack(const std::string& packFile, const std::string& dir) {
    const auto pack = HiresPack::open(packFile);
    if (!pack) return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& [frame, colour] : pack->keys()) {
        const auto bytes = pack->read(frame, colour);
        if (!bytes) return false;
        std::ofstream out(fs::path(dir) / (hiresFrameName(frame, colour) + ".png"), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (!out) return false;
    }
    return true;
}

}  // namespace ab
