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

bool OrderReconciliator::enter_submit_unknown(const OrderIntent& intent, std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pending_.contains(intent.local_order_id)) {
        return false;
    }
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

std::vector<ReconcileResult> OrderReconciliator::poll_open_orders(
    const Ticker& ticker, 
    std::chrono::system_clock::time_point now_time) 
{
    std::vector<ReconcileResult> out;
    std::vector<RestOrder> server_orders;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!fetch_) {
            LOG_ERROR("OrderReconciliator::poll_open_orders called with no fetcher set");
            return {};
        }
        
        auto bt = by_ticker_.find(ticker);
        if (bt == by_ticker_.end() || bt->second.empty()) {
            return out;
        }
    }

    FetchOpenOrders fetcher;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fetcher = fetch_;
    }

    try {
        server_orders = fetcher(ticker);
    } catch (const std::exception& ex) {
        LOG_WARN("OrderReconciliator: fetch failed for ticker={}: {}", ticker, ex.what());
        std::lock_guard<std::mutex> lk(mtx_);
        auto bt = by_ticker_.find(ticker);
        if (bt == by_ticker_.end()) return out;
        
        for (auto local_id : bt->second) {
            auto it = pending_.find(local_id);
            if (it == pending_.end()) continue;
            
            if (now_time < it->second.next_poll_at) continue;

            it->second.current_backoff = std::min(
                cfg_.max_backoff,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    it->second.current_backoff * cfg_.backoff_multiplier));
            it->second.next_poll_at = now_time + it->second.current_backoff;

            out.push_back(ReconcileResult{
                /*outcome=*/ReconcileOutcome::Pending, local_id,
                std::nullopt, it->second.intent, std::nullopt,
            });
        }
        return out;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    auto bt = by_ticker_.find(ticker);
    if (bt == by_ticker_.end() || bt->second.empty()) {
        return out;
    }

    std::vector<int64_t> to_remove;
    std::vector<int64_t> local_ids(bt->second.begin(), bt->second.end());

    for (int64_t local_id : local_ids) {
        auto it = pending_.find(local_id);
        if (it == pending_.end()) {
            continue;  // resolved by a parallel call
        }

        if (now_time < it->second.next_poll_at) {
            out.push_back(ReconcileResult{ReconcileOutcome::Pending, local_id, std::nullopt, it->second.intent, std::nullopt});
            continue;
        }

        auto sit = std::find_if(server_orders.begin(), server_orders.end(), [&](const auto& server) {
            return matches_intent_(it->second.intent, server);
        });
        std::optional<int64_t> matched = (sit != server_orders.end()) ? std::optional<int64_t>{sit->id} : std::nullopt;

        if (matched) {
            out.push_back(ReconcileResult{
                /*outcome=*/ReconcileOutcome::Resolved,
                /*local=*/local_id,
                /*server=*/matched,
                /*intent=*/it->second.intent,
                /*note=*/std::nullopt,
            });
            to_remove.push_back(local_id);
            LOG_INFO("OrderReconciliator: poll matched local_id={} -> server_id={}",
                     local_id, *matched);
        } else {
            const auto elapsed = now_time - it->second.started_at;
            if (elapsed >= cfg_.total_timeout) {
                out.push_back(ReconcileResult{
                    /*outcome=*/ReconcileOutcome::NotFoundTimeout,
                    /*local=*/local_id,
                    /*server=*/std::nullopt,
                    /*intent=*/it->second.intent,
                    /*note=*/std::string{"deadline exceeded; manual intervention required"},
                });
                to_remove.push_back(local_id);
                LOG_ERROR("OrderReconciliator: timeout local_id={} ticker={} -> manual ack required",
                          local_id, ticker);
            } else {
                it->second.current_backoff = std::min(
                    cfg_.max_backoff,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        it->second.current_backoff * cfg_.backoff_multiplier));
                it->second.next_poll_at = now_time + it->second.current_backoff;
                out.push_back(ReconcileResult{
                    /*outcome=*/ReconcileOutcome::Pending,
                    /*local=*/local_id,
                    /*server=*/std::nullopt,
                    /*intent=*/it->second.intent,
                    /*note=*/std::nullopt,
                });
            }
        }
    }

    for (int64_t id : to_remove) {
        auto it_pending = pending_.find(id);
        if (it_pending != pending_.end()) {
            Ticker tkr = it_pending->second.intent.ticker;
            pending_.erase(it_pending);
            
            auto it_tkr = by_ticker_.find(tkr);
            if (it_tkr != by_ticker_.end()) {
                it_tkr->second.erase(id);
                if (it_tkr->second.empty()) {
                    by_ticker_.erase(it_tkr);
                }
            }
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

    if (intent.type != OrderType::Market) {
        const double price_tol = std::max(1e-8, std::abs(intent.price) * (cfg_.price_tolerance_bps / 10'000.0));
        if (std::abs(server.price - intent.price) > price_tol) return false;
    }

    const double size_tol = std::max(1e-8, std::abs(intent.size) * (cfg_.size_tolerance_pct / 100.0));
    if (std::abs(server.size - intent.size) > size_tol) return false;

    return true;
}

} // namespace trade_bot
