#pragma once

#include "transport/external/IExternalFeed.hpp"
#include <memory>
#include <map>
#include <string>

namespace trade_bot {

enum class FeedKind {
    Funding,
    // Add more as needed
};

class ExternalFeedRegistry {
public:
    static ExternalFeedRegistry& instance() {
        static ExternalFeedRegistry inst;
        return inst;
    }

    void register_feed(FeedKind kind, std::shared_ptr<IExternalFeed> feed) {
        feeds_[kind] = std::move(feed);
    }

    void clear() {
        feeds_.clear();
    }

    template<typename T>
    std::shared_ptr<T> get_feed(FeedKind kind) const {
        auto it = feeds_.find(kind);
        if (it != feeds_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    const std::map<FeedKind, std::shared_ptr<IExternalFeed>>& all_feeds() const {
        return feeds_;
    }

private:
    std::map<FeedKind, std::shared_ptr<IExternalFeed>> feeds_;
};

} // namespace trade_bot
