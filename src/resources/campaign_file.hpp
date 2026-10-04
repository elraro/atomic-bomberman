// The original's campaign files (data/res/*.cam): one stage per line,
//   -C,name,level,scheme,rovers,rover speed,ghosts,ghost speed,AIs,AI difficulty
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ab {

struct CampaignStage {
    std::string name;
    int level = 0;
    std::string scheme;  // scheme file name without extension, lower case
    int rovers = 0;
    int roverSpeed = 0;
    int ghosts = 0;
    int ghostSpeed = 0;
    int computerPlayers = 0;
    int difficulty = 0;  // unused by the original
};

std::vector<CampaignStage> parseCampaignText(const std::string& text);
std::optional<std::vector<CampaignStage>> loadCampaignFile(const std::string& path);

}  // namespace ab
