#include "OrderReconciliator.hpp"

#include "logger/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace trade_bot {

OrderReconciliator::OrderReconciliator() : OrderReconciliator(Config{}) {}

OrderReconciliator::OrderReconciliator(const Config& cfg) : cfg_(cfg) {}

void OrderReconciliator::set_fetch_open_orders(FetchOpenOrders fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    fetch_ = std::move(fn);
}

bool OrderReconciliator::enter_submit_unknown(const OrderIntent& intent) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pending_.contains(intent.local_order_id)) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    pending_.emplace(intent.local_order_id, PendingIntent{
        /*intent=*/intent,
        /*started_at=*/now,
        /*next_poll_at=*/now,                    // first poll is immediate
        /*current_backoff=*/cfg_.initial_backoff,
    });
    by_ticker_[intent.ticker].insert(intent.local_order_id);
    LOG_WARN("OrderReconciliator: SubmitUnknown registered ticker={} local_id={} side={} type={} price={} size={}",
             intent.ticker, intent.local_order_id,
             static_cast<int>(intent.side), static_cast<int>(intent.type),
             intent.price, intent.size);
    return true;
}

bool OrderReconciliator::resolve_order(int64_t local_order_id, int64_t server_order_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(local_order_id);
    if (it == pending_.end()) {
        return false;
    }
    const auto ticker = it->second.intent.ticker;
    pending_.erase(it);
    if (auto bt = by_ticker_.find(ticker); bt != by_ticker_.end()) {
        bt->second.erase(local_order_id);
        if (bt->second.empty()) {
            by_ticker_.erase(bt);
        }
    }
    LOG_INFO("OrderReconciliator: resolved local_id={} -> server_id={}",
             local_order_id, server_order_id);
    return true;
}

std::vector<ReconcileResult> OrderReconciliator::poll_open_orders(const Ticker& ticker) {
    std::vector<ReconcileResult> out;

    // Snapshot ids/intents under lock to avoid holding the mutex across HTTP I/O.
    std::vector<PendingIntent> snapshot;
    FetchOpenOrders fetcher;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fetcher = fetch_;
        auto bt = by_ticker_.find(ticker);
        if (bt == by_ticker_.end() || bt->second.empty()) {
            return out;
        }
        snapshot.reserve(bt->second.size());
        for (auto local_id : bt->second) {
            if (auto it = pending_.find(local_id); it != pending_.end()) {
                snapshot.push_back(it->second);
            }
        }
    }

    if (!fetcher) {
        LOG_ERROR("OrderReconciliator::poll_open_orders called with no fetcher set");
        return out;
    }

    const auto now = std::chrono::steady_clock::now();

    // Filter to those whose next_poll_at <= now.
    std::vector<PendingIntent> due;
    due.reserve(snapshot.size());
    std::copy_if(snapshot.begin(), snapshot.end(), std::back_inserter(due), [&](const auto& p) {
        return p.next_poll_at <= now;
    });
    if (due.empty()) {
        return out;
    }

    std::vector<RestOrder> server_orders;
    try {
        server_orders = fetcher(ticker);
    } catch (const std::exception& ex) {
        LOG_WARN("OrderReconciliator: fetch failed for ticker={}: {}", ticker, ex.what());
        // Treat as transient — schedule next backoff for all due intents.
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& p : due) {
            auto it = pending_.find(p.intent.local_order_id);
            if (it == pending_.end()) continue;
            it->second.current_backoff =
                std::min(cfg_.max_backoff,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             it->second.current_backoff * cfg_.backoff_multiplier));
            it->second.next_poll_at = now + it->second.current_backoff;

            const auto elapsed = now - it->second.started_at;
            if (elapsed >= cfg_.total_timeout) {
                out.push_back(ReconcileResult{
                    /*outcome=*/ReconcileOutcome::NotFoundTimeout,
                    /*local=*/p.intent.local_order_id,
                    /*server=*/std::nullopt,
                    /*intent=*/p.intent,
                    /*note=*/std::string{"timeout after fetch errors"},
                });
                pending_.erase(it);
                if (auto bt = by_ticker_.find(ticker); bt != by_ticker_.end()) {
                    bt->second.erase(p.intent.local_order_id);
                    if (bt->second.empty()) by_ticker_.erase(bt);
                }
            } else {
                out.push_back(ReconcileResult{
                    /*outcome=*/ReconcileOutcome::Pending, p.intent.local_order_id,
                    std::nullopt, p.intent, std::nullopt,
                });
            }
        }
        return out;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& p : due) {
        auto it = pending_.find(p.intent.local_order_id);
        if (it == pending_.end()) {
            continue;  // resolved by a parallel call
        }

        // Try to find a matching server order.
        auto sit = std::find_if(server_orders.begin(), server_orders.end(), [&](const auto& server) {
            return matches_intent_(p.intent, server);
        });
        std::optional<int64_t> matched = (sit != server_orders.end()) ? std::optional<int64_t>{sit->id} : std::nullopt;

        if (matched) {
            out.push_back(ReconcileResult{
                /*outcome=*/ReconcileOutcome::Resolved,
                /*local=*/p.intent.local_order_id,
                /*server=*/matched,
                /*intent=*/p.intent,
                /*note=*/std::nullopt,
            });
            pending_.erase(it);
            if (auto bt = by_ticker_.find(ticker); bt != by_ticker_.end()) {
                bt->second.erase(p.intent.local_order_id);
                if (bt->second.empty()) by_ticker_.erase(bt);
            }
            LOG_INFO("OrderReconciliator: poll matched local_id={} -> server_id={}",
                     p.intent.local_order_id, *matched);
            continue;
        }

        // No match — backoff and check timeout.
        const auto elapsed = now - it->second.started_at;
        if (elapsed >= cfg_.total_timeout) {
            out.push_back(ReconcileResult{
                /*outcome=*/ReconcileOutcome::NotFoundTimeout,
                /*local=*/p.intent.local_order_id,
                /*server=*/std::nullopt,
                /*intent=*/p.intent,
                /*note=*/std::string{"deadline exceeded; manual intervention required"},
            });
            pending_.erase(it);
            if (auto bt = by_ticker_.find(ticker); bt != by_ticker_.end()) {
                bt->second.erase(p.intent.local_order_id);
                if (bt->second.empty()) by_ticker_.erase(bt);
            }
            LOG_ERROR("OrderReconciliator: timeout local_id={} ticker={} -> manual ack required",
                      p.intent.local_order_id, ticker);
        } else {
            it->second.current_backoff =
                std::min(cfg_.max_backoff,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             it->second.current_backoff * cfg_.backoff_multiplier));
            it->second.next_poll_at = now + it->second.current_backoff;
            out.push_back(ReconcileResult{
                /*outcome=*/ReconcileOutcome::Pending,
                /*local=*/p.intent.local_order_id,
                /*server=*/std::nullopt,
                /*intent=*/p.intent,
                /*note=*/std::nullopt,
            });
        }
    }

    return out;
}

bool OrderReconciliator::has_pending(const Ticker& ticker) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = by_ticker_.find(ticker);
    return it != by_ticker_.end() && !it->second.empty();
}

size_t OrderReconciliator::pending_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.size();
}

bool OrderReconciliator::matches_intent_(const OrderIntent& intent,
                                        const RestOrder& server) const {
    if (server.ticker != intent.ticker) return false;
    if (server.side != intent.side) return false;
    if (server.type != intent.type) return false;

    // T4-MATCHING: Handle Market orders where price might be 0 in intent (#153)
    if (intent.type != OrderType::Market) {
        const double price_tol = std::max(1e-8, std::abs(intent.price) * (cfg_.price_tolerance_bps / 10'000.0));
        if (std::abs(server.price - intent.price) > price_tol) return false;
    }

    const double size_tol = std::max(1e-8, std::abs(intent.size) * (cfg_.size_tolerance_pct / 100.0));
    if (std::abs(server.size - intent.size) > size_tol) return false;

    return true;
}

} // namespace trade_bot
