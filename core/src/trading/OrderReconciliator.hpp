#pragma once

#include "domain/Types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_bot {

/**
 * T0-ORDER-RECONCILIATION:
 *
 * Resolves ambiguity introduced when a `place_order` HTTP call timed out
 * or returned 5xx after the request body may have been sent. Per
 * METASCALP_API_CONTRACT § 3, blind retry is unsafe: the bot must enter
 * `SubmitUnknown`, pause the ticker, and try to find the order by polling
 * `GET /api/connections/{id}/orders?Ticker=...` with exponential backoff.
 *
 * If a matching server-side order is found, the local tracking id is
 * paired with the server `OrderId`. If the deadline (`total_timeout`)
 * expires without a match, the intent is marked `NotFoundTimeout` so the
 * caller can alert the operator and switch the ticker to read-only.
 */

struct OrderIntent {
    int64_t   local_order_id;   // bot-generated tracking id (for correlation)
    Ticker    ticker;
    Side      side;
    OrderType type;
    double    price;
    double    size;
};

enum class ReconcileOutcome {
    Pending,
    Resolved,
    NotFoundTimeout,
};

struct ReconcileResult {
    ReconcileOutcome           outcome;
    int64_t                    local_order_id;
    std::optional<int64_t>     server_order_id;
    std::optional<OrderIntent> intent;   // original intent, populated for side/size matching
    std::optional<std::string> note;
};

class OrderReconciliator {
public:
    using FetchOpenOrders = std::function<std::vector<RestOrder>(const Ticker&)>;

    struct Config {
        std::chrono::milliseconds initial_backoff{500};
        std::chrono::milliseconds max_backoff{8'000};
        std::chrono::milliseconds total_timeout{30'000};   // 30 s default
        double                    backoff_multiplier{2.0};
        // Matching tolerances (server-side prices may differ slightly due to rounding)
        double                    price_tolerance_bps{2.0};   // ±0.02%
        double                    size_tolerance_pct{0.5};    // ±0.5%
    };

    OrderReconciliator();
    explicit OrderReconciliator(const Config& cfg);

    /// Inject the open-orders fetcher (typically wraps OrderGateway::get_open_orders).
    void set_fetch_open_orders(FetchOpenOrders fn);

    /// Register a local intent as `SubmitUnknown`. Returns false if already tracked.
    bool enter_submit_unknown(const OrderIntent& intent);

    /// Manually resolve `local_order_id` with a known server `OrderId`.
    /// Returns false if the local id was not tracked.
    bool resolve_order(int64_t local_order_id, int64_t server_order_id);

    /// Run one polling round for `ticker`. Returns one result per tracked intent
    /// for this ticker. Resolved/NotFoundTimeout entries are removed from the
    /// tracker after this call.
    std::vector<ReconcileResult> poll_open_orders(const Ticker& ticker);

    bool   has_pending(const Ticker& ticker) const;
    size_t pending_count() const;

private:
    struct PendingIntent {
        OrderIntent                            intent;
        std::chrono::steady_clock::time_point  started_at;
        std::chrono::steady_clock::time_point  next_poll_at;
        std::chrono::milliseconds              current_backoff;
    };

    bool matches_intent_(const OrderIntent& intent, const RestOrder& server) const;

    Config              cfg_;
    FetchOpenOrders     fetch_;
    mutable std::mutex  mtx_;
    std::unordered_map<int64_t, PendingIntent>   pending_;     // local_order_id -> state
    std::unordered_map<Ticker, std::set<int64_t>> by_ticker_;  // ticker -> local_order_ids
};

} // namespace trade_bot
