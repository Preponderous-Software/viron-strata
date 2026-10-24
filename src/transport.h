// The HTTP layer. Transport has one method, get(): the client cannot send a
// write because there is no call to send one with (RFC 0030, section 5).
// viron-usd's client does the same.
#pragma once

#include <map>
#include <memory>
#include <string>

namespace viron_strata {

struct Response {
    int status = 0;                              // 0: no HTTP answer (refused, timeout, DNS)
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    std::string error;                           // set when status == 0
    double millis = 0;                           // time the request took
};

class Transport {
public:
    virtual ~Transport() = default;
    // GET baseUrl + path, with the bearer token if one is set.
    virtual Response get(const std::string& path) = 0;
};

// libcurl. https:// URLs are verified against the system CA store; nothing
// is ever written to disk or logged (the token included).
std::unique_ptr<Transport> makeCurlTransport(const std::string& baseUrl, const std::string& token,
                                             double timeoutSeconds = 5.0);

// Percent-encodes one path segment (an environment name).
std::string urlEncode(const std::string& segment);

}  // namespace viron_strata
