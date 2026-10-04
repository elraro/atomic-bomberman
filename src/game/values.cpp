#include "game/values.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ab {

Values Values::defaults() {
    Values v;
    v.set(10, 10);
    v.set(20, 10);
    v.set(30, 20);
    v.set(31, 150);
    v.set(25, 20);
    v.set(27, 1);
    v.set(41, 40);
    v.set(46, 1);
    v.set(100, 150);
    v.set(101, 60);
    v.set(42, 923);
    const int start[15] = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 15; ++i) v.set(50 + i, start[i]);
    v.set(90, 150);
    v.set(91, 150);
    v.set(102, 40);
    v.set(105, 24);
    v.set(120, 1);
    v.set(123, 1);
    v.set(124, 1);
    v.set(125, 10);
    v.set(129, 10);
    for (int i = 0; i < 9; ++i) v.set(130 + i, 300);
    v.set(300, 1000);
    v.set(301, 1300);
    v.set(667, 3);
    v.set(670, 1);
    v.set(671, 3);
    const int level[13] = {10, 10, 3, 4, 8, 2, 2, 1, -2, -4, 1, -4, -2};
    for (int i = 0; i < 13; ++i) v.set(400 + i, level[i]);
    const int cap[15] = {8, 8, 0, 1, 4, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    for (int i = 0; i < 15; ++i) v.set(550 + i, cap[i]);
    return v;
}

bool Values::loadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

void Values::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (auto semi = line.find(';'); semi != std::string::npos) line.erase(semi);
        const char* p = line.c_str();
        char* end = nullptr;
        long id = std::strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        int offset = 0;
        while (*p == ',' || *p == ' ' || *p == '\t') {
            while (*p == ',' || *p == ' ' || *p == '\t') ++p;
            long value = std::strtol(p, &end, 10);
            if (end == p) break;
            set(static_cast<int>(id) + offset, static_cast<int>(value));
            ++offset;
            p = end;
        }
    }
}

int Values::get(int id) const {
    auto it = table_.find(id);
    if (it == table_.end()) throw std::out_of_range("undefined value id " + std::to_string(id));
    return it->second;
}

}  // namespace ab
