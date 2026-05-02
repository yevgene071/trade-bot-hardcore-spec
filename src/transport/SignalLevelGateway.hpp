#pragma once

#include "IHttpClient.hpp"
#include "domain/Types.hpp"

#include <vector>
#include <string>

namespace trade_bot {

/**
 * REST client for managing signal levels on MetaScalp server.
 */
class SignalLevelGateway {
public:
    SignalLevelGateway(IHttpClient& http, std::string base_url, int connection_id);
    virtual ~SignalLevelGateway() = default;

    virtual std::vector<SignalLevel> get_all(const Ticker& ticker);
    virtual int create(const Ticker& ticker, double price);
    virtual void remove(int id);
    virtual void remove_all(const Ticker& ticker);
    virtual void cleanup_triggered();

private:
    IHttpClient& http_;
    std::string  base_url_;
    int          connection_id_;
};

} // namespace trade_bot
