// Where the game finds the configuration of online play when nothing else is
// said: addresses and key built in when the game was built (CMake options
// AB_ONLINE_CONFIG_URLS and AB_ONLINE_CONFIG_KEY). A copy built without them has
// no online play until the player names a configuration (settings keys
// online_config_url / online_config_key, or the command line).
#pragma once

#include <string>

#include "online/config.hpp"

#ifndef AB_ONLINE_CONFIG_URLS
#define AB_ONLINE_CONFIG_URLS ""
#endif
#ifndef AB_ONLINE_CONFIG_KEY
#define AB_ONLINE_CONFIG_KEY ""
#endif

namespace ab::online {

// The bootstrap to use: what was given (both parts), or else what is built in.
inline bool chooseBootstrap(const std::string& urls, const std::string& keyHex, Bootstrap& out, std::string& error) {
    if (!urls.empty() || !keyHex.empty()) {
        if (urls.empty() || keyHex.empty()) {
            error = "A configuration of online play needs both its address and its key";
            return false;
        }
        return makeBootstrap(urls, keyHex, out, error);
    }
    if (std::string(AB_ONLINE_CONFIG_URLS).empty()) {
        error = "Online play is not set up in this copy of the game (no configuration address; see docs/operations/online.md)";
        return false;
    }
    return makeBootstrap(AB_ONLINE_CONFIG_URLS, AB_ONLINE_CONFIG_KEY, out, error);
}

}  // namespace ab::online
