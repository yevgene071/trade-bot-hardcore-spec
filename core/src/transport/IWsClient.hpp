#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <nlohmann/json.hpp>

namespace trade_bot {

class IWsClient {
public:
    virtual ~IWsClient() = default;

    virtual void connect(const std::string& url) = 0;
    virtual void send(std::string_view message) = 0;
    virtual void disconnect() = 0;

    virtual void set_on_message(std::function<void(const nlohmann::json&)> callback) = 0;
    virtual void set_on_close(std::function<void(int code, const std::string& reason)> callback) = 0;
    virtual void set_on_error(std::function<void(const std::string& msg)> callback) = 0;
    virtual void set_on_connect(std::function<void()> callback) = 0;

    virtual bool is_connected() const = 0;
};

} // namespace trade_bot
