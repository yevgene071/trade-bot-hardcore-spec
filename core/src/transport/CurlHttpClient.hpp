#pragma once

#include "IHttpClient.hpp"
#include <curl/curl.h>
#include <atomic>
#include <mutex>

namespace trade_bot {

class CurlHttpClient : public IHttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    HttpResponse get(const std::string& url) override;
    HttpResponse post(const std::string& url, const std::string& body) override;
    HttpResponse put(const std::string& url, const std::string& body) override;
    HttpResponse del(const std::string& url) override;

    void set_timeout_ms(int timeout_ms) override;

private:
    HttpResponse perform_request(const std::string& url, const std::string& method, const std::string& body = "");
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata);

    CURL* m_curl{nullptr};
    mutable std::mutex m_mtx;
    std::atomic<int> m_timeout_ms{5000};  // O7: atomic for thread-safe set_timeout_ms
};

} // namespace trade_bot
