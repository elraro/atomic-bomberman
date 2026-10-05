#include "net/upnp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "net/socket.hpp"

namespace ab::net {

namespace {

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

int httpStatus(const std::string& response) {
    // "HTTP/1.1 200 OK"
    const auto space = response.find(' ');
    return space == std::string::npos ? 0 : std::atoi(response.c_str() + space + 1);
}

std::string soap(const std::string& host, int port, const std::string& path, const std::string& service, const std::string& action,
                 const std::string& arguments) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                             action + " xmlns:u=\"" + service + "\">" + arguments + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    return "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
           "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action +
           "\"\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

const char* const kSearchTargets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                      "urn:schemas-upnp-org:service:WANPPPConnection:1"};

}  // namespace

std::string httpHeader(const std::string& response, const std::string& name) {
    const std::string wanted = lower(name) + ":";
    std::size_t pos = 0;
    while (pos < response.size()) {
        std::size_t end = response.find('\n', pos);
        if (end == std::string::npos) end = response.size();
        const std::string line = response.substr(pos, end - pos);
        if (trim(line).empty() && pos != 0) break;  // end of the headers
        if (lower(line).rfind(wanted, 0) == 0) return trim(line.substr(wanted.size()));
        pos = end + 1;
    }
    return {};
}

bool splitUrl(const std::string& url, std::string* host, int* port, std::string* path) {
    const std::string scheme = "http://";
    if (lower(url).rfind(scheme, 0) != 0) return false;
    const std::size_t start = scheme.size();
    std::size_t slash = url.find('/', start);
    if (slash == std::string::npos) slash = url.size();
    std::string authority = url.substr(start, slash - start);
    *port = 80;
    if (const auto colon = authority.rfind(':'); colon != std::string::npos && authority.find(']') == std::string::npos) {
        *port = std::atoi(authority.c_str() + colon + 1);
        authority.resize(colon);
    }
    *host = authority;
    *path = slash < url.size() ? url.substr(slash) : "/";
    return !host->empty() && *port > 0 && *port < 65536;
}

std::string xmlValue(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    const auto a = xml.find(open);
    if (a == std::string::npos) return {};
    const auto b = xml.find(close, a);
    if (b == std::string::npos) return {};
    return trim(xml.substr(a + open.size(), b - a - open.size()));
}

bool findWanService(const std::string& xml, std::string* controlUrl, std::string* serviceType) {
    std::size_t pos = 0;
    for (;;) {
        const auto a = xml.find("<service>", pos);
        if (a == std::string::npos) return false;
        auto b = xml.find("</service>", a);
        if (b == std::string::npos) b = xml.size();
        const std::string block = xml.substr(a, b - a);
        const std::string type = xmlValue(block, "serviceType");
        if (type.find(":WANIPConnection:") != std::string::npos || type.find(":WANPPPConnection:") != std::string::npos) {
            *serviceType = type;
            *controlUrl = xmlValue(block, "controlURL");
            if (!controlUrl->empty()) return true;
        }
        pos = b;
    }
}

UpnpResult upnpMapPort(const UpnpTransport& transport, std::uint16_t port, const std::string& description) {
    UpnpResult result;
    // 1. Find the router.
    std::string location;
    for (const char* target : kSearchTargets) {
        const std::string message = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + target + "\r\n\r\n";
        for (const std::string& answer : transport.search(message))
            if (location.empty()) location = httpHeader(answer, "location");
        if (!location.empty()) break;
    }
    if (location.empty()) {
        result.message = "no router answered the UPnP search (it may be switched off on the router)";
        return result;
    }
    std::string host, path;
    int hostPort = 0;
    if (!splitUrl(location, &host, &hostPort, &path)) {
        result.message = "the router gave an address this program cannot use: " + location;
        return result;
    }
    // 2. Its description names the service that maps ports and where to send requests.
    std::string localIp;
    const auto described = transport.http(host, hostPort, "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(hostPort) + "\r\nConnection: close\r\n\r\n", &localIp);
    std::string control, service;
    if (!described || httpStatus(*described) != 200 || !findWanService(*described, &control, &service)) {
        result.message = "the router's description has no port mapping service";
        return result;
    }
    // The control URL may be a whole URL, or a path on the same or another base.
    std::string controlHost = host, controlPath = control;
    int controlPort = hostPort;
    if (lower(control).rfind("http://", 0) == 0) {
        if (!splitUrl(control, &controlHost, &controlPort, &controlPath)) {
            result.message = "the router's control address cannot be used: " + control;
            return result;
        }
    } else {
        if (const std::string base = xmlValue(*described, "URLBase"); !base.empty()) {
            std::string ignored;
            splitUrl(base, &controlHost, &controlPort, &ignored);
        }
        if (controlPath.empty() || controlPath[0] != '/') controlPath = "/" + controlPath;
    }
    if (localIp.empty()) {
        result.message = "this machine's own address could not be worked out";
        return result;
    }
    // 3. One mapping per protocol: the outside port to the same port on this machine.
    for (const char* protocol : {"TCP", "UDP"}) {
        const std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" + protocol +
                                      "</NewProtocol><NewInternalPort>" + std::to_string(port) + "</NewInternalPort><NewInternalClient>" + localIp +
                                      "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + description +
                                      "</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>";
        const auto answer = transport.http(controlHost, controlPort, soap(controlHost, controlPort, controlPath, service, "AddPortMapping", arguments), nullptr);
        if (!answer || httpStatus(*answer) != 200) {
            const std::string why = answer ? xmlValue(*answer, "errorDescription") : std::string();
            result.message = std::string("the router refused to open ") + protocol + " port " + std::to_string(port) + (why.empty() ? "" : " (" + why + ")");
            return result;
        }
    }
    result.ok = true;
    result.host = controlHost;
    result.hostPort = controlPort;
    result.controlPath = controlPath;
    result.service = service;
    if (const auto answer = transport.http(controlHost, controlPort, soap(controlHost, controlPort, controlPath, service, "GetExternalIPAddress", ""), nullptr);
        answer && httpStatus(*answer) == 200)
        result.externalIp = xmlValue(*answer, "NewExternalIPAddress");
    result.message = "the router now passes TCP and UDP port " + std::to_string(port) + " to this machine (" + localIp + ")";
    return result;
}

void upnpUnmapPort(const UpnpTransport& transport, const UpnpResult& mapping, std::uint16_t port) {
    if (!mapping.ok) return;
    for (const char* protocol : {"TCP", "UDP"}) {
        const std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" + protocol + "</NewProtocol>";
        transport.http(mapping.host, mapping.hostPort, soap(mapping.host, mapping.hostPort, mapping.controlPath, mapping.service, "DeletePortMapping", arguments), nullptr);
    }
}

UpnpTransport realUpnpTransport() {
    UpnpTransport t;
    t.search = [](const std::string& message) { return multicastAsk("239.255.255.250", 1900, message, 2500); };
    t.http = [](const std::string& host, int port, const std::string& request, std::string* localIp) { return blockingExchange(host, port, request, 4000, localIp); };
    return t;
}

PortMapper::~PortMapper() { stop(); }

void PortMapper::start(std::uint16_t port, const std::string& description) {
    stop();
    started_ = true;
    finished_ = false;
    port_ = port;
    thread_ = std::thread([this, port, description] {
        const UpnpResult r = upnpMapPort(realUpnpTransport(), port, description);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            result_ = r;
        }
        finished_ = true;
    });
}

UpnpResult PortMapper::result() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

void PortMapper::stop() {
    if (thread_.joinable()) thread_.join();
    if (started_ && result_.ok) upnpUnmapPort(realUpnpTransport(), result_, port_);
    started_ = false;
    result_ = {};
}

}  // namespace ab::net
