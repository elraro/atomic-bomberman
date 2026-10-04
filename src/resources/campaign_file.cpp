#include "resources/campaign_file.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ab {

std::vector<CampaignStage> parseCampaignText(const std::string& text) {
    std::vector<CampaignStage> stages;
    std::istringstream in(text.substr(0, text.find('\x1a')));
    std::string line;
    while (std::getline(in, line)) {
        // As the original (0x401085): a line counts when it starts with "-C" and has
        // exactly ten comma-separated fields.
        if (line.size() < 2 || line[0] != '-' || std::toupper(static_cast<unsigned char>(line[1])) != 'C') continue;
        std::vector<std::string> f;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t comma = line.find(',', pos);
            f.push_back(line.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (f.size() != 10) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            std::size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            return s.substr(i);
        };
        CampaignStage st;
        st.name = trim(f[1]);
        st.level = std::atoi(f[2].c_str());
        st.scheme = trim(f[3]);
        for (char& ch : st.scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        st.rovers = std::atoi(f[4].c_str());
        st.roverSpeed = std::atoi(f[5].c_str());
        st.ghosts = std::atoi(f[6].c_str());
        st.ghostSpeed = std::atoi(f[7].c_str());
        st.computerPlayers = std::atoi(f[8].c_str());
        st.difficulty = std::atoi(f[9].c_str());
        stages.push_back(st);
    }
    return stages;
}

std::optional<std::vector<CampaignStage>> loadCampaignFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseCampaignText(ss.str());
}

}  // namespace ab
