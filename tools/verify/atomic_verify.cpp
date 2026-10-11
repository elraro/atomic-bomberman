// atomic_verify: checks signed match records (docs/specifications/online-play.md,
// section 7) and sums up what they say.
//
//   atomic_verify [--root HEX]... [--revoked N]... [--stats] FILE...
//
// For each file: who signed it, whether a certificate signed by one of the root keys
// vouches for that key, and whether the round, played again from the recorded seed and
// inputs with this program's gameplay core, ends as recorded. A record can only be
// played again by the game version that made it.
//
// --stats adds a table of the players in the records that hold (and, when root keys are
// given, come from servers with a certificate that counts): rounds, rounds won, kills.
// Exit status: 0 if every record holds, 1 otherwise.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "online/record_check.hpp"

int main(int argc, char** argv) {
    using namespace ab;
    std::vector<online::PublicKey> roots;
    std::vector<long long> revoked;
    std::vector<std::string> files;
    bool stats = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) {
            online::PublicKey key{};
            if (!online::fromHex(argv[++i], key.data(), key.size())) {
                std::fprintf(stderr, "ERROR --root needs a key of 64 hexadecimal digits\n");
                return 2;
            }
            roots.push_back(key);
        } else if (a == "--revoked" && i + 1 < argc) {
            revoked.push_back(std::atoll(argv[++i]));
        } else if (a == "--stats") {
            stats = true;
        } else if (a == "--version") {
            std::puts("atomic_verify " AB_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h" || a.rfind("--", 0) == 0) {
            std::puts("Usage: atomic_verify [--root HEX]... [--revoked SERIAL]... [--stats] FILE...");
            return a == "--help" || a == "-h" ? 0 : 2;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        std::puts("Usage: atomic_verify [--root HEX]... [--revoked SERIAL]... [--stats] FILE...");
        return 2;
    }
    struct Tally {
        std::string name;
        int rounds = 0, wins = 0, kills = 0;
    };
    std::map<std::string, Tally> tally;  // by player key, or by name for a player without one
    int bad = 0, counted = 0;
    for (const std::string& file : files) {
        std::ifstream in(file, std::ios::binary);
        const std::string envelope((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const online::RecordReport report = online::examineRecord(envelope, AB_VERSION, roots, revoked);
        const net::MatchRecord& r = report.record;
        if (report.readable)
            std::printf("%s: %s  round %u of \"%s\", %zu steps, signed by %s%s\n", file.c_str(), report.holds ? "HOLDS" : "DOES NOT HOLD", r.start.roundId, r.serverName.c_str(),
                        r.inputs.size(), online::toHex(report.signer).substr(0, 16).c_str(),
                        report.certified ? (" (certificate of \"" + report.operatorName + "\")").c_str() : "");
        else std::printf("%s: DOES NOT HOLD\n", file.c_str());
        for (const std::string& p : report.problems) std::printf("    problem: %s\n", p.c_str());
        for (const std::string& n : report.notes) std::printf("    note: %s\n", n.c_str());
        if (!report.holds) {
            ++bad;
            continue;
        }
        if (!stats || (!roots.empty() && !report.certified)) continue;
        ++counted;
        // Who sat where, and what the round added to the score. (In team play a win is the team's.)
        for (int s = 0; s < kMaxPlayers; ++s) {
            const net::RecordSeat& seat = r.seats[static_cast<std::size_t>(s)];
            if (seat.kind != net::SeatKind::Human) continue;
            for (const net::PlayerLog& p : r.players) {
                if (p.client != seat.client) continue;
                const bool hasKey = p.playerKey != net::Key{};
                const std::string id = hasKey ? net::toHex(p.playerKey) : "name:" + p.name;
                Tally& t = tally[id + (seat.local > 0 ? "#" + std::to_string(seat.local + 1) : "")];
                t.name = p.name + (seat.local > 0 ? " (" + std::to_string(seat.local + 1) + ")" : "") + (hasKey ? "" : " [no key]");
                ++t.rounds;
                const int scoreSlot = r.end.teamPlay ? r.start.setup.teams[static_cast<std::size_t>(s)] : s;
                t.wins += r.end.winner == scoreSlot ? 1 : 0;
                t.kills += r.end.score.kills[static_cast<std::size_t>(s)] - r.start.score.kills[static_cast<std::size_t>(s)];
            }
        }
    }
    std::printf("%zu record(s): %zu hold, %d do not\n", files.size(), files.size() - static_cast<std::size_t>(bad), bad);
    if (stats) {
        std::vector<Tally> rows;
        for (const auto& [id, t] : tally) rows.push_back(t);
        std::sort(rows.begin(), rows.end(), [](const Tally& a, const Tally& b) { return a.wins != b.wins ? a.wins > b.wins : a.kills > b.kills; });
        std::printf("\nPlayers in %d record(s)%s:\n%-28s %7s %7s %7s\n", counted, roots.empty() ? "" : " from servers with a certificate that counts", "player", "rounds", "won", "kills");
        for (const Tally& t : rows) std::printf("%-28s %7d %7d %7d\n", t.name.c_str(), t.rounds, t.wins, t.kills);
    }
    return bad == 0 ? 0 : 1;
}
