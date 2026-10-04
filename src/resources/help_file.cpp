#include "resources/help_file.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace ab {

std::vector<HelpLine> parseHelpText(const std::string& text) {
    std::vector<HelpLine> lines;
    std::string raw;
    auto flush = [&]() {
        // Tabs to the next multiple of four columns.
        std::string expanded;
        for (char ch : raw) {
            if (ch == '\t') {
                do expanded += ' ';
                while (expanded.size() % 4 != 0);
            } else if (ch != '\r') {
                expanded += ch;
            }
        }
        HelpLine line;
        std::size_t pos = 0;
        while (pos < expanded.size()) {
            const std::size_t tag = expanded.find("<IMG", pos);
            const std::size_t close = tag == std::string::npos ? std::string::npos : expanded.find('>', tag);
            if (close == std::string::npos) {
                line.push_back({false, expanded.substr(pos)});
                break;
            }
            if (tag > pos) line.push_back({false, expanded.substr(pos, tag - pos)});
            std::string name = expanded.substr(tag + 4, close - tag - 4);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            line.push_back({true, name});
            pos = close + 1;
        }
        lines.push_back(std::move(line));
        raw.clear();
    };
    for (char ch : text) {
        if (ch == '\x1a') break;
        if (ch == '\n')
            flush();
        else
            raw += ch;
    }
    if (!raw.empty()) flush();
    return lines;
}

std::optional<std::vector<HelpLine>> loadHelpFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseHelpText(ss.str());
}

}  // namespace ab
