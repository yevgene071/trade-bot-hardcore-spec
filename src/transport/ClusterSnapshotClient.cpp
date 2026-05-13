#include "ClusterSnapshotClient.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "utils/UrlEncoder.hpp"

#include <nlohmann/json.hpp>
#include <sstream>

namespace trade_bot {

ClusterSnapshotClient::ClusterSnapshotClient(IHttpClient& http,
                                           std::string base_url,
                                           int connection_id)
    : http_(http)
    , base_url_(std::move(base_url))
    , connection_id_(connection_id) {}

std::optional<ClusterSnapshot> ClusterSnapshotClient::fetch(const Ticker& ticker,
                                                          const std::string& timeframe,
                                                          int zoom_index) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_ << "/cluster-snapshot"
       << "?Ticker=" << url_encode(ticker)
       << "&TimeFrame=" << url_encode(timeframe)
       << "&ZoomIndex=" << zoom_index;

    try {
        auto resp = http_.get(ss.str());
        if (resp.status != 200) {
            LOG_ERROR("ClusterSnapshotClient: failed to fetch for {}: HTTP {}",
                      ticker, resp.status);
            return std::nullopt;
        }

        auto j = nlohmann::json::parse(resp.body);
        return MetaScalpCodec::parse_cluster_snapshot(j);
    } catch (const std::exception& ex) {
        LOG_ERROR("ClusterSnapshotClient: error fetching {}: {}", ticker, ex.what());
        return std::nullopt;
    }
}

} // namespace trade_bot
