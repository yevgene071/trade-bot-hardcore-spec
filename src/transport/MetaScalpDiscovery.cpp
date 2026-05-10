#include "MetaScalpDiscovery.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace trade_bot {

MetaScalpDiscovery::MetaScalpDiscovery(std::shared_ptr<IHttpClient> client)
    : m_client(std::move(client)) {}

std::optional<int> MetaScalpDiscovery::discover() {
    auto start_time = std::chrono::steady_clock::now();
    const int start_port = 17845;
    const int end_port = 17855;
    const int timeout_sec = 5;

    // Set a short timeout for discovery probes
    m_client->set_timeout_ms(500);

    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(timeout_sec)) {
        for (int port = start_port; port <= end_port; ++port) {
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ping";
            auto response = m_client->get(url);

            if (response.status == 200) {
                try {
                    auto j = nlohmann::json::parse(response.body);
                    if (j.contains("app") && j["app"] == "MetaScalp") {
                        std::string version = j.value("version", "unknown");
                        LOG_INFO("MetaScalp API discovered on port {} (Version: {})", port, version);
                        return port;
                    }
                } catch (const std::exception& e) {
                    LOG_DEBUG("Failed to parse /ping response from port {}: {}", port, e.what());
                }
            }
        }
        
        // Brief pause before next scan iteration if not timed out
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_WARN("MetaScalp API discovery timed out (no instance found on ports {}..{})", start_port, end_port);
    return std::nullopt;
}

} // namespace trade_bot
