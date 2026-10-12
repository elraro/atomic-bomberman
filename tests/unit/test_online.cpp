// Tests of the game's side of online play (src/online): the JSON reader, signatures
// and signed documents, the rules a configuration must meet, the HTTP client, and
// the master client against a stand-in server in this process. The real master
// server (Go) and TLS are tried by tests/integration/online.sh.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/net_sockets.h>

#include "fake_http.hpp"
#include "online/certificate.hpp"
#include "online/config.hpp"
#include "online/http.hpp"
#include "online/ice.hpp"
#include "online/json.hpp"
#include "online/online.hpp"
#include "online/sign.hpp"
#include "online/tournament.hpp"

using namespace ab::online;
using fake::FakeServer;
using fake::Reply;
using fake::Request;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_TEXT(a, b)                                                                                              \
    do {                                                                                                              \
        ++g_checks;                                                                                                   \
        const std::string va = (a);                                                                                   \
        const std::string vb = (b);                                                                                   \
        if (va != vb) {                                                                                               \
            ++g_failures;                                                                                             \
            std::printf("  FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, va.c_str(), vb.c_str()); \
        }                                                                                                             \
    } while (0)

void testJson() {
    Json j;
    CHECK(Json::parse(R"( {"a": 1, "b": [true, false, null, -2.5e1, "x\ny\u00e9\ud83d\ude00"], "c": {"d": "e"}, "big": 9007199254740992} )", j));
    CHECK(j.isObject() && j["a"].integer() == 1 && j["b"].items().size() == 5);
    CHECK(j["b"].items()[0].boolean() && !j["b"].items()[1].boolean() && j["b"].items()[2].isNull());
    CHECK(j["b"].items()[3].number() == -25.0);
    CHECK_TEXT(j["b"].items()[4].str(), "x\ny\xC3\xA9\xF0\x9F\x98\x80");
    CHECK_TEXT(j["c"]["d"].str(), "e");
    CHECK(j["big"].integer() == 9007199254740992LL);
    // What is not there reads as nothing, at any depth.
    CHECK(j["nope"]["deeper"].isNull() && j["nope"].str().empty() && j["a"]["x"].integer() == 0 && j["a"].items().empty());
    // Written and read again it is the same.
    Json again;
    CHECK(Json::parse(j.dump(), again));
    CHECK_TEXT(again.dump(), j.dump());
    Json made = Json::object();
    made.set("name", "a \"quoted\" \\ name\t").set("n", 42).set("list", Json::array().push(1).push("two"));
    CHECK_TEXT(made.dump(), R"({"list":[1,"two"],"n":42,"name":"a \"quoted\" \\ name\t"})");
    // Refused: anything that is not exactly one JSON value.
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "{'a':1}", "01", "1 2", "\"unterminated", "\"bad \\x escape\"", "nul", "{\"a\":1,}", "[1 2]", "\"\\ud83d\"", "-", "1.", "1e"}) {
        Json none;
        CHECK(!Json::parse(bad, none));
    }
    std::string deep(100, '[');
    deep += std::string(100, ']');
    Json none;
    CHECK(!Json::parse(deep, none));
    CHECK(!Json::parse(std::string((1u << 20) + 10, ' '), none));
}

void testSignatures() {
    // RFC 8032, section 7.1, the first two test vectors.
    struct Vector {
        const char* seed;
        const char* pub;
        std::string message;
        const char* signature;
    };
    const Vector vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "\x72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    };
    for (const Vector& v : vectors) {
        Seed seed{};
        PublicKey pub{};
        Signature expected{};
        CHECK(fromHex(v.seed, seed.data(), 32) && fromHex(v.pub, pub.data(), 32) && fromHex(v.signature, expected.data(), 64));
        CHECK(publicKeyOf(seed) == pub);
        CHECK(sign(seed, v.message) == expected);
        CHECK(verify(pub, v.message, expected));
        CHECK(!verify(pub, v.message + "x", expected));
        Signature altered = expected;
        altered[10] ^= 1;
        CHECK(!verify(pub, v.message, altered));
    }
    // Hex and base64.
    std::uint8_t two[2];
    CHECK(fromHex("0aFf", two, 2) && two[0] == 10 && two[1] == 255);
    CHECK(!fromHex("0aF", two, 2) && !fromHex("0aFg", two, 2) && !fromHex("0aFf00", two, 2));
    for (const std::string& text : {std::string(), std::string("f"), std::string("fo"), std::string("foo"), std::string("foob"), std::string("\0\xff\x10", 3)}) {
        std::string back;
        CHECK(fromBase64(toBase64(text), back) && back == text);
    }
    CHECK_TEXT(toBase64("foobar"), "Zm9vYmFy");
    CHECK_TEXT(toBase64("fo"), "Zm8=");
    std::string out;
    CHECK(!fromBase64("Zm9", out) && !fromBase64("Zm9v!mFy", out) && !fromBase64("Zm=v", out));
    CHECK_TEXT(sha256Hex(reinterpret_cast<const std::uint8_t*>("abc"), 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // A signed document: opened by who trusts the key, and by nobody once it is touched.
    Seed seed{}, otherSeed{};
    seed.fill(7);
    otherSeed.fill(9);
    const PublicKey key = publicKeyOf(seed), other = publicKeyOf(otherSeed);
    const std::string document = R"({"serial": 3,  "exact bytes":"matter"})";
    const std::string envelope = signDocument(seed, document);
    std::string got, why;
    PublicKey signer{};
    CHECK(openDocument(envelope, {other, key}, &got, &signer, &why) && got == document && signer == key);
    CHECK(!openDocument(envelope, {other}, &got, nullptr, &why));
    CHECK_TEXT(why, "signed by a key that is not trusted");
    CHECK(!openDocument(envelope, {}, &got, nullptr, &why));
    Json env;
    CHECK(Json::parse(envelope, env));
    Json forged = env;
    forged.set("document", toBase64(R"({"serial": 4,  "exact bytes":"matter"})"));
    CHECK(!openDocument(forged.dump(), {key}, &got, nullptr, &why));
    CHECK_TEXT(why, "the signature does not match the document");
    Json stolen;
    CHECK(Json::parse(signDocument(otherSeed, document), stolen));
    stolen.set("key", toHex(key));  // somebody else's signature under the trusted key's name
    CHECK(!openDocument(stolen.dump(), {key}, &got, nullptr, &why));
    for (const char* junk : {"", "[]", "{}", R"({"document":"!!","signature":"","key":""})"}) {
        CHECK(!openDocument(junk, {key}, &got, nullptr, &why));
        CHECK_TEXT(why, "not a signed document");
    }
}

Seed testSeed() {
    Seed s{};
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<std::uint8_t>(i * 7 + 1);
    return s;
}

std::string configDocument(long long serial, long long issued, long long expires, const std::string& master, const std::string& extra = "") {
    return "{\"serial\":" + std::to_string(serial) + ",\"issued\":" + std::to_string(issued) + ",\"expires\":" + std::to_string(expires) +
           ",\"master\":{\"url\":\"" + master + "\",\"tls_pins\":[\"aa\"]},\"stun\":[\"turn.example.org:3478\"],"
           "\"turn\":[{\"host\":\"turn.example.org\",\"port\":3478,\"transport\":\"udp\"},{\"host\":\"turn.example.org\",\"port\":443,\"transport\":\"tls\"},"
           "{\"host\":\"\",\"port\":1,\"transport\":\"udp\"},{\"host\":\"x\",\"port\":3478,\"transport\":\"carrier pigeon\"}],"
           "\"protocol\":{\"min\":8,\"current\":8},\"status\":{\"state\":\"ok\",\"message\":\"hello\"},"
           "\"trust\":{\"roots\":[\"" + toHex(publicKeyOf(testSeed())) + "\",\"not a key\"],\"revoked\":[5,9]},\"flags\":{\"x\":true}" + extra + "}";
}

void testConfigRules() {
    const Seed seed = testSeed();
    const PublicKey key = publicKeyOf(seed);
    const long long now = 1800000000;
    OnlineConfig c;
    std::string why;
    CHECK(readConfig(signDocument(seed, configDocument(12, now - 3600, now + 3600, "https://master.example.org:8443/")), key, now, 0, c, why));
    CHECK(c.serial == 12 && c.masterUrl == "https://master.example.org:8443" && c.masterPins.size() == 1);
    CHECK(c.stun.size() == 1 && c.turn.size() == 2 && c.turn[1].transport == "tls" && c.turn[1].port == 443);
    CHECK(c.protocolMin == 8 && c.statusState == "ok" && c.statusMessage == "hello");
    CHECK(c.roots.size() == 1 && c.roots[0] == key && c.revoked.size() == 2 && c.revoked[1] == 9 && c.flags["x"].boolean());
    auto refused = [&](const std::string& document, long long at, long long lowest, const std::string& reason) {
        OnlineConfig none;
        std::string said;
        const bool ok = readConfig(signDocument(seed, document), key, at, lowest, none, said);
        CHECK(!ok);
        if (said.find(reason) == std::string::npos) {
            ++g_failures;
            std::printf("  FAIL expected a reason with \"%s\", got \"%s\"\n", reason.c_str(), said.c_str());
        }
    };
    const std::string good = configDocument(12, now - 3600, now + 3600, "https://master.example.org");
    refused(good, now + 3600, 0, "expired");                       // at the very second it runs out
    refused(good, now, 13, "older than one already seen");         // a step back
    CHECK(readConfig(signDocument(seed, good), key, now, 12, c, why));  // the same one again is fine
    refused(configDocument(12, now - 3600, now + 8 * 24 * 3600, "https://m"), now, 0, "longer than seven days");
    refused(configDocument(12, now + 3 * 24 * 3600, now + 4 * 24 * 3600, "https://m"), now, 0, "future");
    refused(configDocument(12, now, now - 1, "https://m"), now, 0, "no dates");
    refused("{\"master\":{\"url\":\"https://m\"}}", now, 0, "no dates");
    refused(configDocument(12, now - 1, now + 1, "ftp://m"), now, 0, "no master server");
    refused("[1,2]", now, 0, "not readable");
    // An https master without a pin would be trusted blindly: refused. A plain one needs none.
    std::string noPin = configDocument(12, now - 1, now + 1, "https://m");
    noPin.replace(noPin.find("[\"aa\"]"), 6, "[]");
    refused(noPin, now, 0, "no key for the master");
    std::string plain = configDocument(12, now - 1, now + 1, "http://127.0.0.1:8080");
    plain.replace(plain.find("[\"aa\"]"), 6, "[]");
    CHECK(readConfig(signDocument(seed, plain), key, now, 0, c, why));
    // Signed by somebody else, or not at all.
    Seed stranger{};
    stranger.fill(3);
    CHECK(!readConfig(signDocument(stranger, good), key, now, 0, c, why));
    CHECK(!readConfig(good, key, now, 0, c, why));

    Bootstrap b;
    CHECK(makeBootstrap("https://a.example/config.json, http://b.example/c.json", toHex(key), b, why) && b.urls.size() == 2 && b.key == key);
    CHECK(!makeBootstrap("", toHex(key), b, why));
    CHECK(!makeBootstrap("a.example/config.json", toHex(key), b, why));
    CHECK(!makeBootstrap("https://a.example/config.json", "abcd", b, why));
}

// A server certificate counts only if a trusted root signed it, it is of that kind, within
// its dates, not revoked, and made out to the identity the server has proved.
void testCertificates() {
    const Seed root = testSeed();
    const PublicKey rootKey = publicKeyOf(root);
    Seed otherRoot{};
    otherRoot.fill(5);
    const long long now = 1800000000;
    const std::string identity(64, 'a');
    Seed signing{};
    signing.fill(11);
    auto document = [&](long long serial, long long issued, long long expires, const std::string& kind = "server-certificate", const std::string& id = std::string(64, 'a')) {
        return "{\"kind\":\"" + kind + "\",\"serial\":" + std::to_string(serial) + ",\"issued\":" + std::to_string(issued) + ",\"expires\":" + std::to_string(expires) +
               ",\"operator\":{\"name\":\"Tuesday League\",\"contact\":\"admin@example.org\"},\"identity_key\":\"" + id + "\",\"signing_key\":\"" +
               toHex(publicKeyOf(signing)) + "\"}";
    };
    ServerCertificate cert;
    std::string why;
    const std::string good = signDocument(root, document(77, now - 86400, now + 89 * 86400));
    CHECK(readCertificate(good, {rootKey}, {5, 9}, now, identity, cert, why));
    CHECK(cert.serial == 77 && cert.operatorName == "Tuesday League" && cert.contact == "admin@example.org" && cert.identityKey == identity && cert.signingKey == publicKeyOf(signing));
    CHECK(readCertificate(good, {rootKey}, {}, now, std::string(64, 'A'), cert, why));  // hex in either case
    auto refused = [&](const std::string& envelope, const std::vector<PublicKey>& roots, const std::vector<long long>& revoked, long long at, const std::string& proven, const std::string& reason) {
        ServerCertificate none;
        std::string said;
        const bool ok = readCertificate(envelope, roots, revoked, at, proven, none, said);
        CHECK(!ok);
        if (said.find(reason) == std::string::npos) {
            ++g_failures;
            std::printf("  FAIL expected a reason with \"%s\", got \"%s\"\n", reason.c_str(), said.c_str());
        }
    };
    refused(good, {rootKey}, {}, now + 89 * 86400, identity, "expired");
    refused(good, {rootKey}, {12, 77}, now, identity, "revoked");
    refused(good, {publicKeyOf(otherRoot)}, {}, now, identity, "not trusted");
    refused(good, {}, {}, now, identity, "not trusted");                          // no roots: nothing can be verified
    refused(good, {rootKey}, {}, now, std::string(64, 'b'), "another server");     // shown by a server that is not its owner
    refused(signDocument(otherRoot, document(77, now - 1, now + 1)), {rootKey}, {}, now, identity, "not trusted");
    refused(signDocument(root, document(77, now + 3 * 86400, now + 9 * 86400)), {rootKey}, {}, now, identity, "future");
    refused(signDocument(root, document(77, now - 1, now + 1, "configuration")), {rootKey}, {}, now, identity, "not a server certificate");
    refused(signDocument(root, "{\"kind\":\"server-certificate\",\"serial\":1}"), {rootKey}, {}, now, identity, "incomplete");
    refused(signDocument(root, document(77, now - 1, now + 1, "server-certificate", "abc")), {rootKey}, {}, now, identity, "incomplete");
    refused("{}", {rootKey}, {}, now, identity, "not a signed document");
    // The other way round: a certificate is not taken for a configuration either.
    OnlineConfig config;
    CHECK(!readConfig(good, rootKey, now, 0, config, why));
    CHECK_TEXT(why, "that document is not a configuration");

    // A key file: made once, the same when read again; rubbish is not taken for a key.
    const std::string file = "ab-online-test-key.txt";
    std::remove(file.c_str());
    Seed a{}, b{};
    bool created = false;
    CHECK(loadOrCreateSeed(file, a, &created) && created && a != Seed{});
    CHECK(loadOrCreateSeed(file, b, &created) && !created && a == b);
    {
        FILE* f = std::fopen(file.c_str(), "w");
        std::fputs("not a key\n", f);
        std::fclose(f);
    }
    CHECK(!loadOrCreateSeed(file, b, &created));
    std::remove(file.c_str());
    Seed c{};
    CHECK(loadOrCreateSeed(file, c) && c != a);  // a new one each time one is made
    std::remove(file.c_str());
    CHECK(!loadOrCreateSeed("no-such-folder/x/key.txt", c));
}

// A tournament file: read only if it is signed (by the organiser asked for, where one is),
// consistent, and not an older issue than one seen.
void testTournamentFiles() {
    Seed organiser{}, stranger{};
    organiser.fill(31);
    stranger.fill(32);
    const PublicKey key = publicKeyOf(organiser);
    auto hex = [](char c) { return std::string(64, c); };
    auto document = [&](long long serial, const std::string& matches, const std::string& kind = "tournament", const std::string& players = "") {
        return "{\"kind\":\"" + kind + "\",\"name\":\"Cup\",\"organiser\":\"Org\",\"serial\":" + std::to_string(serial) + ",\"issued\":5,"
               "\"rules\":{\"game_version\":\"1.0\",\"protocol\":10,\"wins\":3,\"level\":-1,\"scheme\":\"x\",\"play_time\":90,\"team_play\":true,\"computers\":2},"
               "\"players\":[" + (players.empty() ? "{\"name\":\"Ann\",\"key\":\"" + hex('1') + "\"},{\"name\":\"Bob\",\"key\":\"" + hex('2') + "\"},{\"name\":\"Cy\",\"key\":\"" + hex('3') + "\"}" : players) + "],"
               "\"servers\":[{\"name\":\"main\",\"address\":\"play.example.org:27410\",\"identity_key\":\"" + hex('A') + "\",\"signing_key\":\"" + hex('b') + "\"}],"
               "\"matches\":[" + matches + "]}";
    };
    const std::string matches =
        "{\"id\":\"R1-1\",\"round\":1,\"players\":[\"Ann\",\"Bob\"],\"server\":\"main\",\"time\":100,\"winner\":\"Bob\",\"records\":[{\"sha256\":\"" + hex('c') + "\",\"url\":\"https://x/1\"}]},"
        "{\"id\":\"R2-1\",\"round\":2,\"players\":[\"Bob\",\"Cy\"],\"server\":\"main\",\"time\":200,\"winner\":\"\",\"records\":null},"
        "{\"id\":\"R3-1\",\"round\":3,\"players\":[\"@R2-1\",\"Ann\"],\"server\":\"\",\"winner\":\"\"}";
    Tournament t;
    std::string why;
    CHECK(readTournament(signDocument(organiser, document(7, matches)), &key, 7, t, why));
    CHECK(t.name == "Cup" && t.organiser == "Org" && t.serial == 7 && t.organiserKey == key);
    CHECK(t.rules.wins == 3 && t.rules.level == -1 && t.rules.scheme == "x" && t.rules.playTime == 90 && t.rules.teamPlay && t.rules.computers == 2 && t.rules.gameVersion == "1.0" && t.rules.protocol == 10);
    CHECK(t.players.size() == 3 && t.servers.size() == 1 && t.matches.size() == 3);
    CHECK(t.servers[0].identityKey == hex('a'));  // in small letters, as the game compares it
    CHECK(t.match("R1-1")->winner == "Bob" && t.match("R1-1")->records.size() == 1 && t.match("R2-1")->records.empty() && t.match("nope") == nullptr);
    // Whose turn: Bob and Cy have the second round; Ann's next match waits for it.
    PublicKey ann{}, bob{}, cy{}, nobody{};
    fromHex(hex('1'), ann.data(), 32);
    fromHex(hex('2'), bob.data(), 32);
    fromHex(hex('3'), cy.data(), 32);
    nobody.fill(9);
    CHECK(t.playerByKey(bob)->name == "Bob" && t.playerByKey(nobody) == nullptr && t.player("Cy") != nullptr && t.player("Dan") == nullptr);
    CHECK(t.openMatchesOf(bob).size() == 1 && t.openMatchesOf(bob)[0]->id == "R2-1" && t.ready(*t.openMatchesOf(bob)[0]));
    CHECK(t.openMatchesOf(ann).size() == 1 && t.openMatchesOf(ann)[0]->id == "R3-1" && !t.ready(*t.openMatchesOf(ann)[0]));
    CHECK(t.openMatchesOf(nobody).empty());
    // Without a key asked for it is read too, and says who signed: the reader then decides.
    CHECK(readTournament(signDocument(stranger, document(1, matches)), nullptr, 0, t, why) && t.organiserKey == publicKeyOf(stranger));
    auto refused = [&](const std::string& envelope, const PublicKey* trusted, long long lowest, const std::string& reason) {
        Tournament none;
        std::string said;
        const bool ok = readTournament(envelope, trusted, lowest, none, said);
        CHECK(!ok);
        if (said.find(reason) == std::string::npos) {
            ++g_failures;
            std::printf("  FAIL expected a reason with \"%s\", got \"%s\"\n", reason.c_str(), said.c_str());
        }
    };
    refused(signDocument(stranger, document(7, matches)), &key, 0, "not signed by the organiser");
    refused(signDocument(organiser, document(6, matches)), &key, 7, "older issue");
    refused(signDocument(organiser, document(7, matches, "configuration")), &key, 0, "not a tournament file");
    refused(document(7, matches), &key, 0, "not a signed document");
    refused("rubbish", nullptr, 0, "not a signed document");
    auto bad = [&](const std::string& brokenMatches, const std::string& reason, const std::string& players = "") {
        refused(signDocument(organiser, document(7, brokenMatches, "tournament", players)), &key, 0, reason);
    };
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"Zed\"],\"server\":\"main\"}", "names a player the tournament does not list");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"@Q\"],\"server\":\"main\"}", "names a player the tournament does not list");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"@M\"],\"server\":\"main\"}", "names a player the tournament does not list");
    bad("{\"id\":\"M\",\"players\":[\"Ann\"],\"server\":\"main\"}", "two to ten players");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"Bob\"],\"server\":\"elsewhere\"}", "names a server");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"Bob\"],\"server\":\"main\",\"winner\":\"Cy\"}", "did not play in it");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"Bob\"]},{\"id\":\"M\",\"players\":[\"Ann\",\"Cy\"]}", "without an id of its own");
    bad("{\"id\":\"M\",\"players\":[\"Ann\",\"Bob\"],\"records\":[{\"sha256\":\"xyz\"}]}", "without its hash");
    bad(matches, "without a name of their own or without a key", "{\"name\":\"Ann\",\"key\":\"" + hex('1') + "\"},{\"name\":\"Ann\",\"key\":\"" + hex('2') + "\"}");
    bad(matches, "one key for two players", "{\"name\":\"Ann\",\"key\":\"" + hex('1') + "\"},{\"name\":\"Bob\",\"key\":\"" + hex('1') + "\"},{\"name\":\"Cy\",\"key\":\"" + hex('3') + "\"}");
    bad(matches, "without a name of their own or without a key", "{\"name\":\"@R1-1\",\"key\":\"" + hex('1') + "\"}");

    std::string location, keyHex;
    splitInvitation("https://example.org/cup.json#" + hex('d') + "\n", location, keyHex);
    CHECK(location == "https://example.org/cup.json" && keyHex == hex('d'));
    splitInvitation("/home/me/cup.ab", location, keyHex);
    CHECK(location == "/home/me/cup.ab" && keyHex.empty());
    CHECK_TEXT(keyFingerprint(key).substr(4, 1), "-");
    CHECK(keyFingerprint(key).size() == 19 && keyFingerprint(key) != keyFingerprint(publicKeyOf(stranger)));
    std::string envelope;
    CHECK(!fetchTournament("no-such-file.ab", envelope, why) && why.find("cannot read") != std::string::npos);
}

// ICE (libjuice) as a carrier: two agents on this machine find each other directly, and,
// when both hide their addresses, through a relay.
void testIce() {
    using ab::ice::Agent;
    const std::string description =
        "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdefghijkl\r\n"
        "a=candidate:1 1 UDP 2122317823 192.168.1.20 40000 typ host\r\n"
        "a=candidate:2 1 UDP 1686109951 203.0.113.9 40000 typ srflx raddr 0.0.0.0 rport 0\r\n"
        "a=candidate:3 1 UDP 25108223 198.51.100.4 50001 typ relay raddr 0.0.0.0 rport 0\r\n"
        "a=end-of-candidates\r\n";
    const std::string hidden = ab::ice::relayOnly(description);
    CHECK(hidden.find("192.168.1.20") == std::string::npos && hidden.find("203.0.113.9") == std::string::npos);
    CHECK(hidden.find("198.51.100.4") != std::string::npos && hidden.find("a=ice-ufrag:abcd") != std::string::npos && hidden.find("a=ice-pwd:") != std::string::npos &&
          hidden.find("a=end-of-candidates") != std::string::npos);

    auto wait = [](const std::function<bool()>& done, int maxMs = 15000) {
        for (int waited = 0; waited < maxMs && !done(); waited += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    };
    auto exchange = [&](Agent& a, Agent& b, const ab::ice::Config& config) {
        if (!a.start(config) || !b.start(config)) return false;
        if (!wait([&] { return a.gathered() && b.gathered(); })) return false;
        if (a.localDescription().empty() || !b.setRemoteDescription(a.localDescription()) || !a.setRemoteDescription(b.localDescription())) return false;
        return wait([&] { return a.state() == Agent::State::Connected && b.state() == Agent::State::Connected; }, 30000);
    };
    auto talk = [&](Agent& a, Agent& b) {
        // A hundred packets each way; all on one machine, so all of them arrive.
        for (int i = 0; i < 100; ++i) {
            a.send(std::vector<std::uint8_t>(600, static_cast<std::uint8_t>(i)));
            b.send(std::vector<std::uint8_t>(900, static_cast<std::uint8_t>(i + 100)));
        }
        int gotA = 0, gotB = 0;
        bool intact = true;
        wait([&] {
            std::vector<std::uint8_t> p;
            while (b.receive(p)) intact = intact && p.size() == 600 && p[0] == static_cast<std::uint8_t>(gotB) && p[599] == p[0], ++gotB;
            while (a.receive(p)) intact = intact && p.size() == 900 && p[0] == static_cast<std::uint8_t>(gotA + 100), ++gotA;
            return gotA == 100 && gotB == 100;
        }, 5000);
        return intact && gotA == 100 && gotB == 100;
    };
    {
        // No server at all: the machine's own addresses are enough.
        Agent a, b;
        CHECK(a.state() == Agent::State::New && !a.gathered() && a.localDescription().empty());
        const bool connected = exchange(a, b, {});
        CHECK(connected);
        if (connected) {
            CHECK(a.alive() && b.alive());
            CHECK_TEXT(a.kind(), "Direct");
            CHECK(a.remote().find(':') != std::string::npos && a.remote() != "unknown:0");
            CHECK(talk(a, b));
        }
    }
    {
        // Both sides hide their addresses: only the relay's are exchanged, and the way goes through it.
        ab::ice::TestServer relay("player", "secret");
        CHECK(relay.port() > 0);
        ab::ice::Config config;
        config.turn.push_back({"127.0.0.1", relay.port(), "player", "secret"});
        config.relayOnly = true;
        Agent a, b;
        const bool connected = exchange(a, b, config);
        CHECK(connected);
        if (connected) {
            // What each side tells the other names the relay only.
            for (const Agent* agent : {&a, &b}) {
                const std::string said = agent->localDescription();
                CHECK(said.find(" typ host") == std::string::npos && said.find(" typ srflx") == std::string::npos && said.find(" typ relay") != std::string::npos);
            }
            CHECK(talk(a, b));
            // (Not checked: that the way taken is the relayed one. On one machine the relay and
            // both players have the same address, so the relay lets a player's own address
            // through as if it were the relay's, and a direct way can form after all. Between
            // different machines the relay refuses packets from addresses it was not told about.)
        }
    }
    {
        // With a relay but nothing to hide, the direct way is still the one taken.
        ab::ice::TestServer relay("player", "secret");
        ab::ice::Config config;
        config.stunHost = "127.0.0.1";
        config.stunPort = relay.port();
        config.turn.push_back({"127.0.0.1", relay.port(), "player", "secret"});
        Agent a, b;
        const bool connected = exchange(a, b, config);
        CHECK(connected);
        if (connected) {
            CHECK(a.localDescription().find(" typ relay") != std::string::npos);
            // (The first pair of addresses that works may be a relayed one; the agents then move to the best.)
            CHECK(wait([&] { return a.kind() == "Direct" && b.kind() == "Direct"; }, 10000));
            CHECK(talk(a, b));
        }
    }
    {
        // A relay that does not know us: no relayed address, and with addresses hidden no way at all.
        ab::ice::TestServer relay("player", "secret");
        ab::ice::Config config;
        config.turn.push_back({"127.0.0.1", relay.port(), "player", "wrong"});
        config.relayOnly = true;
        Agent a;
        CHECK(a.start(config));
        CHECK(wait([&] { return a.gathered(); }, 30000));
        CHECK(a.localDescription().find(" typ relay") == std::string::npos);
    }
}

// A relay that is reached over TCP, for the test: libjuice's own (UDP) behind a listener
// that does the bridge's work the other way round. It cuts the stream with the same two
// functions, so what this shows is that the bridge carries a whole session; that a real
// relay reads the stream the same way is shown against coturn (tests/integration/online.sh).
class StreamRelay {
public:
    StreamRelay(const std::string& address, int udpPort) : address_(address) {
        // (A port of our choosing: the library does not say which one the system would give.)
        for (int port = 47000 + static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000); port_ == 0 && port < 49000; ++port) {
            mbedtls_net_init(&listen_);
            if (mbedtls_net_bind(&listen_, "127.0.0.1", std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP) == 0) port_ = port;
        }
        if (port_ == 0) return;
        mbedtls_net_set_nonblock(&listen_);
        thread_ = std::thread([this, udpPort] { run(udpPort); });
    }
    ~StreamRelay() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        mbedtls_net_free(&listen_);
    }
    int port() const { return port_; }
    int connections() const { return connections_.load(); }
    int channelData() const { return channelData_.load(); }  // packets of the players' own that came in by the stream

private:
    struct Pair {
        mbedtls_net_context tcp, udp;
        std::vector<std::uint8_t> in;
    };
    void run(int udpPort) {
        std::vector<std::unique_ptr<Pair>> pairs;
        unsigned char buf[4096];
        std::vector<std::uint8_t> datagram, out;
        while (!stop_.load()) {
            auto pair = std::make_unique<Pair>();
            mbedtls_net_init(&pair->tcp);
            mbedtls_net_init(&pair->udp);
            if (mbedtls_net_accept(&listen_, &pair->tcp, nullptr, 0, nullptr) == 0 &&
                mbedtls_net_connect(&pair->udp, address_.c_str(), std::to_string(udpPort).c_str(), MBEDTLS_NET_PROTO_UDP) == 0) {
                mbedtls_net_set_nonblock(&pair->tcp);
                mbedtls_net_set_nonblock(&pair->udp);
                pairs.push_back(std::move(pair));
                connections_.fetch_add(1);
            }
            for (auto& p : pairs) {
                int n = 0;
                while ((n = mbedtls_net_recv(&p->tcp, buf, sizeof buf)) > 0) {
                    p->in.insert(p->in.end(), buf, buf + n);
                    while (ab::ice::turnFromStream(p->in, datagram) == 1) {
                        mbedtls_net_send(&p->udp, datagram.data(), datagram.size());
                        if ((datagram[0] & 0xC0) == 0x40) channelData_.fetch_add(1);
                    }
                }
                while ((n = mbedtls_net_recv(&p->udp, buf, sizeof buf)) > 0) {
                    out.clear();
                    ab::ice::turnToStream(buf, static_cast<std::size_t>(n), out);
                    for (std::size_t at = 0; at < out.size() && !stop_.load();) {
                        const int sent = mbedtls_net_send(&p->tcp, out.data() + at, out.size() - at);
                        if (sent > 0) at += static_cast<std::size_t>(sent);
                        else if (sent != MBEDTLS_ERR_SSL_WANT_WRITE) break;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& p : pairs) {
            mbedtls_net_free(&p->tcp);
            mbedtls_net_free(&p->udp);
        }
    }
    mbedtls_net_context listen_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0}, channelData_{0};
    std::string address_;
    int port_ = 0;
};

// The relay over TCP: how TURN's messages lie in a stream, and a whole session through the bridge.
void testTurnStream() {
    using ab::ice::Agent;
    using ab::ice::turnFromStream;
    using ab::ice::turnToStream;
    std::vector<std::uint8_t> stream, datagram;
    // A STUN message says its own length (here 8 bytes after the header of 20) and goes in as it is.
    std::vector<std::uint8_t> stun(28, 0);
    stun[0] = 0x00, stun[1] = 0x03, stun[3] = 8;
    CHECK(turnToStream(stun.data(), stun.size(), stream) && stream == stun);
    // Channel data of 5 bytes: 9 bytes as a datagram, filled up to 12 in the stream.
    const std::vector<std::uint8_t> channel = {0x40, 0x01, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    CHECK(turnToStream(channel.data(), channel.size(), stream) && stream.size() == 28 + 12 && stream[37] == 0 && stream[39] == 0);
    // One of 4 bytes needs no filling.
    const std::vector<std::uint8_t> even = {0x7F, 0xFE, 0x00, 0x04, 1, 2, 3, 4};
    CHECK(turnToStream(even.data(), even.size(), stream) && stream.size() == 28 + 12 + 8);
    // Neither of the two, too short, or a length that is not the datagram's: not passed on.
    const std::vector<std::uint8_t> other = {0x80, 0x00, 0x00, 0x00}, cut = {0x40, 0x01, 0x00, 0x09, 1, 2};
    const std::size_t before = stream.size();
    CHECK(!turnToStream(other.data(), other.size(), stream) && !turnToStream(cut.data(), cut.size(), stream) && !turnToStream(stun.data(), 27, stream) &&
          !turnToStream(stun.data(), 3, stream) && stream.size() == before);
    // Out again, whatever pieces the stream arrives in.
    std::vector<std::uint8_t> arriving;
    std::vector<std::vector<std::uint8_t>> got;
    for (const std::uint8_t byte : stream) {
        arriving.push_back(byte);
        int r = 0;
        while ((r = turnFromStream(arriving, datagram)) == 1) got.push_back(datagram);
        CHECK(r == 0);
    }
    CHECK(got.size() == 3 && arriving.empty());
    if (got.size() == 3) CHECK(got[0] == stun && got[1] == channel && got[2] == even);
    std::vector<std::uint8_t> nonsense = {0xC0, 0, 0, 0};
    CHECK(turnFromStream(nonsense, datagram) == -1);

    auto wait = [](const std::function<bool()>& done, int maxMs) {
        for (int waited = 0; waited < maxMs && !done(); waited += 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    };
    {
        // One player reaches the relay over TCP alone, the other over UDP; both hide their
        // addresses, so the way between them is the relayed one, and the packets cross the bridge.
        // (Where the machine answers to a second address of its own, the relay sits there:
        // it then refuses what a player sends it directly, as a relay elsewhere would, and
        // the way through the bridge is the only one. Elsewhere a direct way can form.)
#ifdef __linux__
        const std::string where = "127.0.0.2";
#else
        const std::string where = "127.0.0.1";
#endif
        ab::ice::TestServer relay("player", "secret", where);
        StreamRelay overTcp(where, relay.port());
        CHECK(overTcp.port() > 0);
        ab::ice::Config udp, tcp;
        udp.turn.push_back({where, relay.port(), "player", "secret"});
        tcp.turn.push_back({"127.0.0.1", overTcp.port(), "player", "secret", "tcp"});
        udp.relayOnly = tcp.relayOnly = true;
        Agent a, b;
        CHECK(a.start(tcp) && b.start(udp));
        CHECK(wait([&] { return a.gathered() && b.gathered(); }, 30000));
        CHECK(overTcp.connections() == 1);
        CHECK(a.reachedOutside() && a.localDescription().find(" typ relay") != std::string::npos);
        CHECK(b.setRemoteDescription(a.localDescription()) && a.setRemoteDescription(b.localDescription()));
        const bool connected = wait([&] { return a.state() == Agent::State::Connected && b.state() == Agent::State::Connected; }, 30000);
        CHECK(connected);
        if (connected) {
            // A hundred packets each way, a few at a time: these are datagrams, and a burst
            // larger than a socket's buffer loses some where a reader is slow (seen on the
            // Windows ARM64 runner with all 200 at once).
            for (int i = 0; i < 100; ++i) {
                a.send(std::vector<std::uint8_t>(601 + i % 4, static_cast<std::uint8_t>(i)));  // (lengths that need filling, and one that does not)
                b.send(std::vector<std::uint8_t>(900, static_cast<std::uint8_t>(i + 100)));
                if (i % 5 == 4) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int gotA = 0, gotB = 0;
            bool intact = true;
            wait([&] {
                std::vector<std::uint8_t> p;
                while (b.receive(p)) intact = intact && p.size() == static_cast<std::size_t>(601 + gotB % 4) && p[0] == static_cast<std::uint8_t>(gotB) && p.back() == p[0], ++gotB;
                while (a.receive(p)) intact = intact && p.size() == 900 && p[0] == static_cast<std::uint8_t>(gotA + 100), ++gotA;
                return gotA == 100 && gotB == 100;
            }, 10000);
            CHECK(intact && gotA == 100 && gotB == 100);
            if (gotA != 100 || gotB != 100) std::printf("  FAIL (details) %d of 100 came back to the player on the stream, %d of 100 reached the other\n", gotA, gotB);
#ifdef __linux__
            CHECK_TEXT(a.kind(), "Relay (TCP)");
            CHECK(overTcp.channelData() >= 100);
#endif
        }
    }
    {
        // Nothing listens where the relay should be: no relayed address, and nothing outside was reached.
        ab::ice::Config config;
        config.turn.push_back({"127.0.0.1", 9, "player", "secret", "tcp"});
        Agent a;
        CHECK(a.start(config));
        CHECK(wait([&] { return a.gathered(); }, 30000));
        CHECK(!a.reachedOutside());
    }
}

void testHttp() {
    bool tls = false;
    std::string host, port, path;
    CHECK(splitUrl("https://master.example.org/v1/status?x=1", &tls, &host, &port, &path) && tls && host == "master.example.org" && port == "443" && path == "/v1/status?x=1");
    CHECK(splitUrl("http://127.0.0.1:8080", &tls, &host, &port, &path) && !tls && host == "127.0.0.1" && port == "8080" && path == "/");
    CHECK(splitUrl("http://[::1]:99/a", &tls, &host, &port, &path) && host == "::1" && port == "99");
    CHECK(splitUrl("https://[2001:db8::1]/a", &tls, &host, &port, &path) && host == "2001:db8::1" && port == "443");
    for (const char* bad : {"ftp://x/", "master.example.org", "http://", "http://host:port/", "http://host:123456/", "http://ho st/", "http://host/a\r\nX: y", "http://[::1/"})
        CHECK(!splitUrl(bad, &tls, &host, &port, &path));

    Request seen;
    FakeServer server([&](const Request& q) {
        seen = q;
        Reply r;
        if (q.path == "/length") r.body = "{\"n\":1}";
        else if (q.path == "/chunked") r.body = "{\"text\":\"a body that comes in several pieces\"}", r.chunked = true;
        else if (q.path == "/until-close") r.body = "{\"n\":3}", r.noLength = true;
        else if (q.path == "/missing") r.status = 404, r.body = "{\"error\":\"No such thing\"}";
        else if (q.path == "/hang-up") r.hangUp = true;
        else if (q.path == "/echo") r.body = q.body;
        return r;
    });
    HttpRequest q;
    q.url = server.url() + "/length";
    HttpResponse r = httpFetch(q);
    CHECK(r.ok() && r.status == 200 && r.body == "{\"n\":1}" && r.error.empty());
    CHECK(seen.method == "GET" && seen.headers["host"] == "127.0.0.1:" + server.url().substr(server.url().rfind(':') + 1));
    q.url = server.url() + "/chunked";
    r = httpFetch(q);
    CHECK(r.ok() && r.body == "{\"text\":\"a body that comes in several pieces\"}");
    q.url = server.url() + "/until-close";
    r = httpFetch(q);
    CHECK(r.ok() && r.body == "{\"n\":3}");
    q.url = server.url() + "/missing";
    r = httpFetch(q);
    CHECK(!r.ok() && r.status == 404 && r.body.find("No such thing") != std::string::npos);
    q.url = server.url() + "/hang-up";
    r = httpFetch(q);
    CHECK(r.status == 0 && !r.error.empty());
    // What is sent: the method, the body with its length, the headers given; a header that
    // would smuggle in a second line is left out.
    q = HttpRequest{};
    q.method = "POST";
    q.url = server.url() + "/echo";
    q.body = "{\"hello\":\"there\"}";
    q.headers = {{"Authorization", "Bearer abc"}, {"X-Bad", "a\r\nX-Injected: yes"}};
    r = httpFetch(q);
    CHECK(r.ok() && r.body == q.body && seen.method == "POST" && seen.headers["authorization"] == "Bearer abc" && seen.headers["content-length"] == "17");
    CHECK(seen.headers.count("x-injected") == 0 && seen.headers.count("x-bad") == 0);
    // Nobody there.
    q = HttpRequest{};
    q.url = "http://127.0.0.1:1/";
    r = httpFetch(q);
    CHECK(r.status == 0 && r.error.find("cannot reach") != std::string::npos);
    // The same on a thread of its own.
    HttpJob job;
    CHECK(!job.running() && !job.finished());
    q.url = server.url() + "/length";
    job.start(q);
    for (int i = 0; i < 2000 && !job.finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(job.finished());
    r = job.take();
    CHECK(r.ok() && r.body == "{\"n\":1}" && !job.finished());
}

// The master client against a stand-in master: configuration from the second address when
// the first serves rubbish, a session, the list, a code, joining, announcing a game.
void testMasterClient() {
    const Seed seed = testSeed();
    long long now = 1800000000;
    std::string configBody;  // what /config.json serves
    int sessions = 0, lobbyPuts = 0;
    bool lobbyKnown = false;
    std::string lastToken, lastHostToken, lastLobbyBody;
    std::string selfUrl;
    FakeServer master([&](const Request& q) {
        Reply r;
        if (q.headers.count("authorization")) lastToken = q.headers.at("authorization");
        if (q.path == "/rubbish.json") r.body = "this is not a configuration";
        else if (q.path == "/config.json") r.body = configBody;
        else if (q.path == "/v1/session" && q.method == "POST") {
            ++sessions;
            r.status = 201;
            r.body = "{\"token\":\"tok" + std::to_string(sessions) + "\",\"id\":\"sess" + std::to_string(sessions) + "\",\"name\":\"Ann\"}";
        } else if (lastToken != "Bearer tok" + std::to_string(sessions)) {
            r.status = 401, r.body = "{\"error\":\"No session; start one first\"}";
        } else if (q.path == "/v1/lobbies" && q.method == "GET") {
            r.body = R"({"lobbies":[{"id":"l1","name":"Open game","players":2,"seats":10,"phase":"lobby","level":"Green Acres","version":8,"region":"eu","certified":true},
                        {"id":"l2","name":"Another","players":1,"seats":4,"phase":"round","level":"","version":8,"region":"","certified":false}],"total":2,"page":0,"page_size":50})";
        } else if (q.path == "/v1/lobbies/by-code/123456") {
            r.body = R"({"id":"l9","name":"Private","players":1,"seats":10,"phase":"lobby","level":"x","version":8,"region":"","certified":false})";
        } else if (q.path.rfind("/v1/lobbies/by-code/", 0) == 0) {
            r.status = 404, r.body = "{\"error\":\"No game with that code\"}";
        } else if (q.path == "/v1/lobbies/l9/join") {
            r.body = q.body.find("123456") != std::string::npos
                         ? R"({"lobby":{"id":"l9","name":"Private"},"candidates":["203.0.113.5:27410","192.168.1.20:27410"],"identity_key":"ab12","certificate":"","relay":{"username":"u","password":"p","ttl":3600,"urls":["turn:t:3478"]}})"
                         : "";
            if (r.body.empty()) r.status = 404, r.body = "{\"error\":\"No such lobby\"}";
        } else if (q.path == "/v1/lobbies" && q.method == "POST") {
            lobbyKnown = true;
            lastLobbyBody = q.body;
            r.status = 201, r.body = R"({"id":"mine","code":"654321","host_token":"host-secret"})";
        } else if (q.path == "/v1/lobbies/mine" && q.method == "PUT") {
            lastHostToken = q.headers.count("x-host-token") ? q.headers.at("x-host-token") : "";
            lastLobbyBody = q.body;
            if (lobbyKnown && lastHostToken == "host-secret") ++lobbyPuts, r.body = "{\"code\":\"654321\"}";
            else r.status = 404, r.body = "{\"error\":\"No such lobby\"}";
        } else if (q.path == "/v1/lobbies/mine" && q.method == "DELETE") {
            lobbyKnown = false;
            r.body = "{}";
        } else {
            r.status = 404, r.body = "{\"error\":\"?\"}";
        }
        return r;
    });
    selfUrl = master.url();
    auto issue = [&](long long serial, long long issued, long long expires, const std::string& extra = "") {
        std::string doc = configDocument(serial, issued, expires, selfUrl, extra);
        doc.replace(doc.find("[\"aa\"]"), 6, "[]");
        configBody = signDocument(seed, doc);
    };
    issue(100, now - 60, now + 3600);
    const std::string serialFile = "ab-online-test-serial.txt";
    std::remove(serialFileFor(serialFile, publicKeyOf(seed)).c_str());

    Online online;
    online.setClock([&] { return now; });
    auto settle = [&](Online& o) {
        for (int i = 0; i < 5000; ++i) {
            o.update();
            if (!o.busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    // Not set up: said so, and nothing is tried.
    online.refreshLobbies();
    CHECK(!online.busy() && online.error().find("not set up") != std::string::npos);
    CHECK(master.requests() == 0);

    Bootstrap bootstrap;
    std::string why;
    CHECK(makeBootstrap("http://127.0.0.1:1/config.json " + selfUrl + "/rubbish.json " + selfUrl + "/config.json", toHex(publicKeyOf(seed)), bootstrap, why));
    online.configure(bootstrap, serialFile, "Ann", 8);
    CHECK(online.configured() && !online.ready());
    online.refreshLobbies();
    CHECK(online.busy());
    CHECK(settle(online));
    CHECK_TEXT(online.error(), "");
    CHECK(online.ready() && online.config().serial == 100 && online.sessionId() == "sess1");
    CHECK(online.lobbies().size() == 2 && online.lobbiesTotal() == 2 && online.lobbiesSerial() == 1);
    CHECK(online.lobbies()[0].id == "l1" && online.lobbies()[0].name == "Open game" && online.lobbies()[0].players == 2 && online.lobbies()[0].certified && online.lobbies()[1].phase == "round");
    // The highest serial seen is kept under the configuration's key: another configuration,
    // signed by somebody else, has a number of its own.
    const std::string kept = serialFileFor(serialFile, publicKeyOf(seed));
    CHECK_TEXT(kept, "ab-online-test-serial-" + toHex(publicKeyOf(seed)).substr(0, 16) + ".txt");
    CHECK_TEXT(serialFileFor("some.dir/serial", publicKeyOf(seed)), "some.dir/serial-" + toHex(publicKeyOf(seed)).substr(0, 16));
    CHECK(serialFileFor("", publicKeyOf(seed)).empty());
    CHECK(readSerial(kept) == 100 && readSerial(serialFile) == 0);
    // A second request reuses configuration and session.
    const int before = master.requests();
    online.refreshLobbies();
    CHECK(settle(online) && master.requests() == before + 1 && sessions == 1 && online.lobbiesSerial() == 2);

    // By code: found, or not (with the master's words).
    LobbyInfo found;
    online.findByCode("123456");
    CHECK(settle(online) && online.takeFound(found) && found.id == "l9" && found.name == "Private" && !online.takeFound(found));
    online.findByCode("000000");
    CHECK(settle(online) && !online.takeFound(found));
    CHECK_TEXT(online.error(), "No game with that code");
    // Joining gives the ways to the game server and how to know it.
    JoinInfo joined;
    online.join("l9", "123456");
    CHECK(settle(online) && online.error().empty() && online.takeJoin(joined));
    CHECK(joined.candidates.size() == 2 && joined.candidates[0] == "203.0.113.5:27410" && joined.identityKey == "ab12" && joined.relayUser == "u" && joined.relayUrls.size() == 1);
    online.join("l9", "999999");
    CHECK(settle(online) && !online.takeJoin(joined) && online.error() == "No such lobby");

    // A host: announced once, then kept up; announced anew when the master has forgotten it.
    LobbyDesc desc;
    desc.name = "Ann's game";
    desc.listed = false;
    desc.players = 1;
    desc.identityKey = std::string(64, 'a');
    desc.port = 27410;
    online.announce(desc);
    CHECK(settle(online) && online.error().empty() && online.announced());
    CHECK_TEXT(online.code(), "654321");
    CHECK(lastLobbyBody.find("\"public\":false") != std::string::npos && lastLobbyBody.find("\"port\":27410") != std::string::npos);
    desc.players = 3;
    desc.phase = "round";
    online.announce(desc);
    CHECK(settle(online) && lobbyPuts == 1 && lastHostToken == "host-secret" && lastLobbyBody.find("\"players\":3") != std::string::npos);
    lobbyKnown = false;  // the master was restarted
    online.announce(desc);
    CHECK(settle(online) && !online.announced() && online.error().empty());
    online.announce(desc);
    CHECK(settle(online) && online.announced() && lobbyKnown);
    online.withdraw();
    CHECK(settle(online) && !online.announced() && !lobbyKnown);

    // The master has forgotten the session: the request fails, the next one opens a new session.
    ++sessions;  // what the client holds is no longer the newest
    online.refreshLobbies();
    CHECK(settle(online) && !online.error().empty() && !online.ready());
    online.refreshLobbies();
    CHECK(settle(online) && online.error().empty() && online.ready() && online.sessionId() == "sess3");

    // When the configuration runs out a new one is fetched; an older one than was seen is refused,
    // and with no valid one there is no online play: nothing is kept from before.
    now += 7200;
    issue(99, now - 60, now + 3600);
    online.refreshLobbies();
    CHECK(settle(online) && !online.ready());
    CHECK(online.error().find("no valid configuration") != std::string::npos && online.error().find("older than one already seen") != std::string::npos);
    issue(101, now - 60, now + 3600);
    online.refreshLobbies();
    CHECK(settle(online) && online.error().empty() && online.ready() && online.config().serial == 101);
    CHECK(readSerial(kept) == 101);
    // Maintenance, and a game too old, are said plainly.
    now += 7200;
    issue(102, now - 60, now + 3600);
    configBody = signDocument(seed, [&] {
        std::string doc = configDocument(102, now - 60, now + 3600, selfUrl);
        doc.replace(doc.find("[\"aa\"]"), 6, "[]");
        const std::string was = "\"state\":\"ok\",\"message\":\"hello\"";
        doc.replace(doc.find(was), was.size(), "\"state\":\"maintenance\",\"message\":\"back at noon\"");
        return doc;
    }());
    online.refreshLobbies();
    CHECK(settle(online) && !online.ready() && online.error().find("maintenance") != std::string::npos && online.error().find("back at noon") != std::string::npos);
    Online old;
    old.setClock([&] { return now; });
    issue(103, now - 60, now + 3600);
    old.configure(bootstrap, "", "Old", 7);
    old.refreshLobbies();
    CHECK(settle(old) && !old.ready() && old.error().find("too old") != std::string::npos);
    std::remove(kept.c_str());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"json", testJson},
        {"signatures and signed documents", testSignatures},
        {"configuration rules", testConfigRules},
        {"server certificates and key files", testCertificates},
        {"tournament files", testTournamentFiles},
        {"ice", testIce},
        {"turn stream", testTurnStream},
        {"http client", testHttp},
        {"master client", testMasterClient},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name.c_str());
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
