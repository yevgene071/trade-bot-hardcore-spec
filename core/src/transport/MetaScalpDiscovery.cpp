#include "MetaScalpDiscovery.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>

namespace trade_bot {

MetaScalpDiscovery::MetaScalpDiscovery(std::shared_ptr<IHttpClient> client)
    : m_client(std::move(client)) {}

std::optional<int> MetaScalpDiscovery::discover() {
    auto start_time = std::chrono::steady_clock::now();
    const int start_port = 17845;
    const int end_port = 17855;
    const int timeout_sec = 15; // Increased total timeout

    // Set a longer timeout for discovery probes (matches JS SDK default)
    m_client->set_timeout_ms(1000);

    LOG_INFO("Starting MetaScalp discovery on ports {}..{} (timeout {}s)", start_port, end_port, timeout_sec);

    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(timeout_sec)) {
        for (int port = start_port; port <= end_port; ++port) {
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/ping";
            auto response = m_client->get(url);

            if (response.status == 200) {
                try {
                    auto j = nlohmann::json::parse(response.body);
                    std::string app_name;
                    if (j.contains("app")) {
                        app_name = j["app"];
                    } else if (j.contains("App")) {
                        app_name = j["App"];
                    }

                    // Check name (case-insensitive for safety)
                    std::string app_name_lower = app_name;
                    std::transform(app_name_lower.begin(), app_name_lower.end(), app_name_lower.begin(), 
                                   [](unsigned char c){ return std::tolower(c); });

                    if (app_name_lower == "metascalp") {
                        std::string version = "unknown";
                        if (j.contains("version")) {
                            version = j["version"];
                        } else if (j.contains("Version")) {
                            version = j["Version"];
                        }
                        LOG_INFO("MetaScalp API discovered on port {} (Version: {})", port, version);
                        return port;
                    } else {
                        LOG_DEBUG("Port {} responded but app is '{}', expected 'MetaScalp'", port, app_name);
                    }
                } catch (const std::exception& e) {
                    LOG_DEBUG("Failed to parse /ping response from port {}: body='{}', error={}", port, response.body, e.what());
                }
            } else if (response.status != -1) {
                LOG_DEBUG("Port {} returned HTTP {} (URL: {})", port, response.status, url);
            }
        }
        
        // Brief pause before next scan iteration if not timed out
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_WARN("MetaScalp API discovery timed out (no instance found on ports {}..{})", start_port, end_port);
    return std::nullopt;
}

} // namespace trade_bot
