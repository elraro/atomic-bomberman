// How the game decides that a TLS server (the master, or the relay over TLS) is the one the
// signed configuration means (docs/specifications/online-play.md, section 2). Two ways, and
// either is enough:
// - a pin: SHA-256 of the server key's SubjectPublicKeyInfo, as before;
// - roots: certificates of authorities (PEM), for instance Let's Encrypt's. The server's
//   chain must lead to one of them, be within its dates, and be made out to the host name
//   the game connected to.
// The system's own list of authorities is never consulted: the configuration names the
// roots, so a new root is a newly signed configuration, not a new release of the game.
#pragma once

#include <string>
#include <vector>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace ab::online {

struct TlsTrust {
    std::vector<std::string> pins;   // hex, any case
    std::vector<std::string> roots;  // PEM, one certificate each
    bool empty() const { return pins.empty() && roots.empty(); }
};

// Reads the roots into `chain` (initialised by the caller). False, with the reason, if one
// of them is not a readable certificate of an authority.
bool loadTlsRoots(const std::vector<std::string>& roots, mbedtls_x509_crt* chain, std::string& error);

// After the handshake (made with VERIFY_OPTIONAL, the roots as the CA chain and the host
// name set): is this the server? `pin` receives the pin of the key it showed. An empty
// trust accepts any server (only for what is checked by its own signature).
bool judgeTlsPeer(const mbedtls_ssl_context* ssl, const TlsTrust& trust, std::string* pin, std::string& error);

// The pin of a key as `judgeTlsPeer` computes it, from a certificate in PEM (for tests and tools).
std::string tlsPinOfPem(const std::string& pem);

}  // namespace ab::online
