#pragma once

#include "IHttpClient.hpp"
#include <optional>
#include <memory>

namespace trade_bot {

class MetaScalpDiscovery {
public:
    explicit MetaScalpDiscovery(std::shared_ptr<IHttpClient> client);

    /**
     * @brief Scans ports 17845..17855 to find MetaScalp API.
     * @return The first found port, or nullopt if none found within timeout.
     */
    std::optional<int> discover();

private:
    std::shared_ptr<IHttpClient> m_client;
};

} // namespace trade_bot
