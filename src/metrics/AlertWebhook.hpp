#pragma once

#include "transport/IHttpClient.hpp"
#include <string>
#include <memory>

namespace trade_bot {

/**
 * T4-METRICS: Simple POST webhook for alerts.
 */
class AlertWebhook {
public:
    AlertWebhook(std::shared_ptr<IHttpClient> http_client, const std::string& url);
    
    void send_alert(const std::string& message);

private:
    std::shared_ptr<IHttpClient> http_client_;
    std::string url_;
};

} // namespace trade_bot
