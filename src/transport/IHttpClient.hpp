#pragma once

#include <string>
#include <map>
#include <vector>

namespace trade_bot {

struct HttpResponse {
    int status;
    std::string body;
    std::map<std::string, std::string> headers;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    virtual HttpResponse get(const std::string& url) = 0;
    virtual HttpResponse post(const std::string& url, const std::string& body) = 0;
    virtual HttpResponse put(const std::string& url, const std::string& body) = 0;
    virtual HttpResponse del(const std::string& url) = 0;

    virtual void set_timeout_ms(int timeout_ms) = 0;
};

} // namespace trade_bot
