#include "resources/unused_voices.hpp"

#include <cctype>
#include <cstring>

namespace ab {

namespace {

// A family of recordings (the letters a file name starts with, before its number) and the
// series the original's own sound list uses that family for: in at least two thirds of
// the lines that name it, or, for a family the list never names (marked), the series of
// the same recordings by another voice.
struct Family {
    const char* letters;
    int series;
};
constexpr Family kFamilies[] = {
    {"add", 700},   {"jdd", 700},   {"zaa", 700},                    // taunts after a death
    {"pgapu", 400}, {"zahpu", 400}, {"jgap", 400 /* never named */},  // a powerup picked up
    {"pupp", 1400}, {"jupp", 1400}, {"zafp", 1400}, {"sofm", 1400}, {"jofm", 1400 /* never named */},  // "awesome"
    {"dmb", 1200},  {"jmb", 1200},  {"zae", 1200},                   // a long string of bombs
    {"pgad", 2300}, {"zahd", 2300}, {"jgad", 2300},                  // a disease caught
    {"jjfm", 2000},                                                  // the match is won
};
// Clips with a name of their own, which the list never names: by their names, taunts.
constexpr const char* kTaunts[] = {"asskill", "bustass", "myass1", "yoassgrs", "brakhead", "kisswook", "jesus", "godhere", "hailgon"};

}  // namespace

int unusedVoiceSeries(const std::string& name) {
    for (const char* taunt : kTaunts)
        if (name == taunt) return 700;
    std::size_t letters = 0;
    while (letters < name.size() && std::isalpha(static_cast<unsigned char>(name[letters])) != 0) ++letters;
    // A family's files are its letters and a number (add36a, zafp13aa).
    if (letters == name.size() || std::isdigit(static_cast<unsigned char>(name[letters])) == 0) return 0;
    for (const Family& f : kFamilies)
        if (std::strlen(f.letters) == letters && name.compare(0, letters, f.letters) == 0) return f.series;
    return 0;
}

}  // namespace ab
