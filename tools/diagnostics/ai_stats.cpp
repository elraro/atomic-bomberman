// Developer tool: plays rounds between computer players and prints how they end.
// Used for the figures in docs/reverse-engineering/ai.md.
//   ai_stats [rounds] [players]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game/ai.hpp"
#include "game/world.hpp"

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 200;
    const int players = argc > 2 ? std::atoi(argv[2]) : 4;
    int single = 0, together = 0, byClock = 0, deaths = 0, ownBomb = 0, bombs = 0;
    long long steps = 0;
    for (int r = 0; r < rounds; ++r) {
        ab::Scheme scheme = ab::Scheme::pillars();
        for (auto& row : scheme.tiles)
            for (ab::Tile& t : row)
                if (t == ab::Tile::Blank) t = ab::Tile::Brick;
        scheme.brickDensity = 90;
        ab::World w{ab::Values::defaults(), static_cast<std::uint32_t>(1000 + r)};
        w.startRound(scheme, true);
        std::vector<ab::AiPlayer> ai;
        for (int i = 0; i < players; ++i) w.addPlayer(i), ai.emplace_back(static_cast<std::uint32_t>(r * 131 + i * 977 + 5));
        while (!w.roundOver()) {
            std::array<ab::PlayerInput, ab::kMaxPlayers> in{};
            for (int i = 0; i < players; ++i) in[static_cast<std::size_t>(i)] = ai[static_cast<std::size_t>(i)].decide(w, i, 50);
            w.tick(50, in);
            for (const ab::Event& e : w.takeEvents()) bombs += e.kind == ab::EventKind::BombDropped ? 1 : 0;
            ++steps;
        }
        if (w.winner() >= 0) ++single;
        else if (w.timeUp()) ++byClock;
        else ++together;
        for (int i = 0; i < players; ++i)
            if (!w.player(i).alive) {
                ++deaths;
                ownBomb += w.player(i).killedBy == i ? 1 : 0;
            }
    }
    std::printf("rounds=%d players=%d single_survivor=%d draws_together=%d draws_by_clock=%d mean_seconds=%.1f deaths=%d by_own_bomb=%d bombs_per_round=%.1f\n",
                rounds, players, single, together, byClock, static_cast<double>(steps) * 0.05 / rounds, deaths, ownBomb, static_cast<double>(bombs) / rounds);
    return 0;
}
