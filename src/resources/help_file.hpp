// The original's help pages (*.bm): plain text with inline pictures written
// as <IMGNAME>, where NAME is a .pcx picture in data/res.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ab {

struct HelpSegment {
    bool image = false;
    std::string text;  // the text, or the picture name in lower case
};

using HelpLine = std::vector<HelpSegment>;

// Lines of a help page. Tabs are expanded to columns of four, as the original's
// viewer does (0x41302D); reading stops at a Ctrl-Z end-of-file mark.
std::vector<HelpLine> parseHelpText(const std::string& text);
std::optional<std::vector<HelpLine>> loadHelpFile(const std::string& path);

}  // namespace ab
