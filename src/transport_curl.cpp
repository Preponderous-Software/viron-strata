#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>

#include <curl/curl.h>

#include "transport.h"

namespace viron_strata {

namespace {

std::once_flag g_curlInit;

size_t onBody(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(user);
    const std::string line(data, size * count);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        std::string value = line.substr(colon + 1);
        const size_t a = value.find_first_not_of(" \t");
        const size_t b = value.find_last_not_of(" \t\r\n");
        (*headers)[name] = a == std::string::npos ? "" : value.substr(a, b - a + 1);
    }
    return size * count;
}

class CurlTransport : public Transport {
public:
    CurlTransport(std::string baseUrl, const std::string& token, double timeoutSeconds)
        : base_(std::move(baseUrl)), timeout_(timeoutSeconds) {
        std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
        curl_ = curl_easy_init();
        if (!token.empty()) auth_ = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
        accept_ = curl_slist_append(auth_, "Accept: application/json");
    }
    ~CurlTransport() override {
        if (accept_) curl_slist_free_all(accept_);
        if (curl_) curl_easy_cleanup(curl_);
    }

    Response get(const std::string& path) override {
        Response r;
        if (!curl_) {
            r.error = "libcurl failed to initialise";
            return r;
        }
        const auto start = std::chrono::steady_clock::now();
        const std::string url = base_ + path;
        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);  // the only method this class sends
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, accept_);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ * 1000));
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min(timeout_, 3.0) * 1000));
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &r.headers);
        const CURLcode code = curl_easy_perform(curl_);
        if (code != CURLE_OK) {
            r.status = 0;
            r.error = curl_easy_strerror(code);
            r.body.clear();
        } else {
            long status = 0;
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
            r.status = static_cast<int>(status);
        }
        r.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return r;
    }

private:
    std::string base_;
    double timeout_;
    CURL* curl_ = nullptr;
    curl_slist* auth_ = nullptr;
    curl_slist* accept_ = nullptr;
};

}  // namespace

std::unique_ptr<Transport> makeCurlTransport(const std::string& baseUrl, const std::string& token,
                                             double timeoutSeconds) {
    return std::make_unique<CurlTransport>(baseUrl, token, timeoutSeconds);
}

}  // namespace viron_strata
