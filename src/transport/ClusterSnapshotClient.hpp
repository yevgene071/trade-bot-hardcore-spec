#pragma once

#include "IHttpClient.hpp"
#include "domain/Types.hpp"

#include <string>
#include <vector>

namespace trade_bot {

/**
 * REST client for fetching cluster snapshots from MetaScalp.
 */
class ClusterSnapshotClient {
public:
    ClusterSnapshotClient(IHttpClient& http, std::string base_url, int connection_id);
    virtual ~ClusterSnapshotClient() = default;

    /// Fetches a cluster snapshot for the given ticker and timeframe.
    /// TimeFrame: "M1", "M5", "M15", "H1", "D1", etc.
    /// ZoomIndex: optional zoom level.
    virtual std::optional<ClusterSnapshot> fetch(const Ticker& ticker,
                                                const std::string& timeframe,
                                                int zoom_index = 0);

private:
    IHttpClient& http_;
    std::string  base_url_;
    int          connection_id_;
};

} // namespace trade_bot
