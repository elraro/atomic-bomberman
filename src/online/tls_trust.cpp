#include "online/tls_trust.hpp"

#include <cctype>

#include "online/sign.hpp"

namespace ab::online {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Why the chain was not taken, in words a player can pass on.
std::string chainProblem(std::uint32_t flags) {
    std::string why;
    auto add = [&](const char* text) { why += (why.empty() ? "" : ", ") + std::string(text); };
    if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) add("it is made out to another name");
    if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) add("it is not from an authority the configuration names");
    if (flags & MBEDTLS_X509_BADCERT_EXPIRED) add("it has expired");
    if (flags & MBEDTLS_X509_BADCERT_FUTURE) add("it is not valid yet (is this computer's clock right?)");
    if (why.empty()) {
        char buf[256];
        const int n = mbedtls_x509_crt_verify_info(buf, sizeof buf, "", flags);
        why = n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : "its certificate does not hold";
        while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
        for (char& c : why)
            if (c == '\n') c = ';';
    }
    return why;
}

}  // namespace

bool loadTlsRoots(const std::vector<std::string>& roots, mbedtls_x509_crt* chain, std::string& error) {
    for (const std::string& pem : roots) {
        if (pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            error = "a root in the configuration is not a certificate";
            return false;
        }
        // PEM is parsed with its terminating NUL counted in.
        const int r = mbedtls_x509_crt_parse(chain, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1);
        if (r != 0) {
            error = "a root in the configuration cannot be read";
            return false;
        }
    }
    return true;
}

bool judgeTlsPeer(const mbedtls_ssl_context* ssl, const TlsTrust& trust, std::string* pin, std::string& error) {
    const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(ssl);
    if (peer == nullptr || peer->pk_raw.p == nullptr) {
        error = "the server showed no certificate";
        return false;
    }
    const std::string shown = sha256Hex(peer->pk_raw.p, peer->pk_raw.len);
    if (pin != nullptr) *pin = shown;
    if (trust.empty()) return true;
    for (const std::string& p : trust.pins)
        if (lower(p) == shown) return true;
    if (!trust.roots.empty()) {
        const std::uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
        if (flags == 0) return true;
        error = "this is not the server the configuration names (" + chainProblem(flags) + ")";
        return false;
    }
    error = "this is not the server the configuration names (its key does not match)";
    return false;
}

std::string tlsPinOfPem(const std::string& pem) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    std::string out;
    if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) == 0)
        out = sha256Hex(crt.pk_raw.p, crt.pk_raw.len);
    mbedtls_x509_crt_free(&crt);
    return out;
}

}  // namespace ab::online
