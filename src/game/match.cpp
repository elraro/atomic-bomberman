#include "game/match.hpp"

namespace ab {

void applyRoundSetup(World& world, const Values& base, const RoundSetup& setup) {
    // Settings the core reads as tuning values.
    world.setValue(vid::kEnclosementDepth, setup.enclosementDepth);
    world.setValue(vid::kWallsDetonateBombs, setup.stompedBombsDetonate ? 1 : 0);
    world.setValue(vid::kDiseasesDestroyable, setup.diseasesDestroyable ? 1 : 0);
    world.setValue(vid::kRoundSeconds, setup.playTime);
    world.setWinByKills(setup.winByKills);
    world.setLevelRules(base.has(450 + setup.level) ? base.get(450 + setup.level) : 0,
                        base.has(340 + setup.level) ? base.get(340 + setup.level) : 0);
    // Scheme powerup rules: a born-with above zero replaces the starting amount, an
    // override replaces the level's count, and forbidden types are kept out of the
    // random powerup. Other types keep the game's own values.
    std::array<bool, 13> forbidden{};
    for (int t = 0; t < 13; ++t) {
        const SchemePower& pw = setup.powers[static_cast<std::size_t>(t)];
        world.setValue(vid::kStartInventory + t, pw.bornWith > 0 ? pw.bornWith : base.get(vid::kStartInventory + t));
        world.setValue(vid::kLevelCount + t, pw.hasOverride ? pw.overrideValue : base.get(vid::kLevelCount + t));
        forbidden[static_cast<std::size_t>(t)] = pw.forbidden;
    }
    world.setForbiddenRandom(forbidden);
    world.setCampaign(setup.campaign);
    world.startRound(setup.scheme, true);
    if (setup.playTime >= kInfinitePlayTime) world.setRoundSeconds(-1);
    world.setExtras(setup.extras, setup.conveyorSpeed);
    world.setTeamPlay(setup.teamPlay, setup.teams);
    for (int i = 0; i < kMaxPlayers; ++i)
        if (setup.present[static_cast<std::size_t>(i)]) world.addPlayer(i);
    for (int i = 0; i < kMaxPlayers; ++i)
        if (world.player(i).present) world.setHuman(i, setup.human[static_cast<std::size_t>(i)]);
    if (setup.campaign) {
        world.spawnAliens(AlienType::Ghost, setup.ghosts, setup.ghostSpeed);
        world.spawnAliens(AlienType::Rover, setup.rovers, setup.roverSpeed);
    }
    for (int i = 0; i < kMaxPlayers; ++i)
        if (world.player(i).present && setup.prize[static_cast<std::size_t>(i)] >= 0) world.grantPrize(i, setup.prize[static_cast<std::size_t>(i)]);
}

int MatchScore::roundDecided(const World& world, int winsNeeded, bool winByKills) {
    const int win = world.teamPlay() ? world.winningTeam() : world.winner();
    if (win >= 0) ++wins[static_cast<std::size_t>(win)];
    for (int i = 0; i < kMaxPlayers; ++i)
        if (world.player(i).present) kills[static_cast<std::size_t>(i)] += world.player(i).kills;
    if (winByKills && !world.teamPlay()) {
        int best = -1000, holders = 0, who = -1;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (!world.player(i).present) continue;
            const int k = kills[static_cast<std::size_t>(i)];
            if (k > best) best = k, holders = 1, who = i;
            else if (k == best) ++holders;
        }
        if (best >= winsNeeded && holders == 1) matchWinner = who;
    } else if (win >= 0 && wins[static_cast<std::size_t>(win)] >= winsNeeded) {
        matchWinner = win;
    }
    return win;
}

}  // namespace ab
