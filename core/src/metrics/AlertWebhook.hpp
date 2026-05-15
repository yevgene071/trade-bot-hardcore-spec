#pragma once

#include "transport/IHttpClient.hpp"
#include <string>
#include <memory>

namespace trade_bot {

/**
 * T4-METRICS: POST webhook for alerts with retry and pluggable body format.
 */
class AlertWebhook {
public:
    enum class Format {
        Telegram, // {"text": "...", "parse_mode": "Markdown"}
        Slack,    // {"text": "..."}
        Generic,  // plain text body
    };

    AlertWebhook(std::shared_ptr<IHttpClient> http_client,
                 const std::string& url,
                 Format format = Format::Telegram,
                 int max_retries = 3);

    void send_alert(const std::string& message);

    int delivered() const noexcept { return delivered_; }
    int failed()    const noexcept { return failed_; }

private:
    std::string build_body_(const std::string& message) const;

    std::shared_ptr<IHttpClient> http_client_;
    std::string url_;
    Format      format_;
    int         max_retries_;
    int         delivered_{0};
    int         failed_{0};
};

} // namespace trade_bot
