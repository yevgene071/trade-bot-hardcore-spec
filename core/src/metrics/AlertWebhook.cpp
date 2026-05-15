#include "AlertWebhook.hpp"
#include "transport/external/ExternalIoContext.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

namespace trade_bot {

AlertWebhook::AlertWebhook(std::shared_ptr<IHttpClient> http_client,
                           const std::string& url,
                           Format format,
                           int max_retries)
    : http_client_(std::move(http_client))
    , url_(url)
    , format_(format)
    , max_retries_(max_retries) {}

std::string AlertWebhook::build_body_(const std::string& message) const {
    switch (format_) {
        case Format::Telegram: {
            nlohmann::json body;
            body["text"] = "⚠️ *Trade Bot Alert*\n" + message;
            body["parse_mode"] = "Markdown";
            return body.dump();
        }
        case Format::Slack: {
            nlohmann::json body;
            body["text"] = message;
            return body.dump();
        }
        case Format::Generic:
            return message;
    }
    return message;
}

void AlertWebhook::send_alert(const std::string& message) {
    if (url_.empty()) return;

    auto& ext_ioc = ExternalIoContext::instance().context();
    boost::asio::post(ext_ioc, [this, message, url = url_]() {
        const std::string body = build_body_(message);
        constexpr int kBaseDelayMs = 1000;

        for (int attempt = 0; attempt < max_retries_; ++attempt) {
            if (attempt > 0) {
                // Exponential backoff: 1s, 3s, 9s, ...
                int delay_ms = kBaseDelayMs;
                for (int i = 0; i < attempt; ++i) delay_ms *= 3;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }

            try {
                auto res = http_client_->post(url, body);
                if (res.status >= 200 && res.status < 300) {
                    ++delivered_;
                    return;
                }
                if (res.status >= 400 && res.status < 500) {
                    // Client error — don't retry
                    LOG_ERROR("AlertWebhook: terminal error {}, body: {}", res.status, res.body);
                    ++failed_;
                    return;
                }
                LOG_WARN("AlertWebhook: attempt {}/{} status {}", attempt + 1, max_retries_, res.status);
            } catch (const std::exception& e) {
                LOG_WARN("AlertWebhook: attempt {}/{} exception: {}", attempt + 1, max_retries_, e.what());
            }
        }

        LOG_ERROR("AlertWebhook: all {} attempts failed for message: {}", max_retries_, message);
        ++failed_;
    });
}

} // namespace trade_bot
