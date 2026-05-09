#include "AlertWebhook.hpp"
#include "transport/external/ExternalIoContext.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

namespace trade_bot {

AlertWebhook::AlertWebhook(std::shared_ptr<IHttpClient> http_client, const std::string& url)
    : http_client_(std::move(http_client)), url_(url) {}

void AlertWebhook::send_alert(const std::string& message) {
    if (url_.empty()) return;

    // Fix for #144: Offload blocking HTTP POST to a background thread
    auto& ext_ioc = ExternalIoContext::instance().context();
    boost::asio::post(ext_ioc, [this, message, url = url_]() {
        nlohmann::json body;
        body["text"] = "⚠️ *Trade Bot Alert*\n" + message;
        body["parse_mode"] = "Markdown";

        try {
            auto res = http_client_->post(url, body.dump());
            if (res.status != 200) {
                LOG_ERROR("AlertWebhook failed: status {}, body: {}", res.status, res.body);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("AlertWebhook exception: {}", e.what());
        }
    });
}

} // namespace trade_bot
