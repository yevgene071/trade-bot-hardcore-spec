#include "DashboardServer.hpp"
#include "generated/market_data_generated.h"
#include "domain/Types.hpp"
#include "strategy/IStrategy.hpp"
#include "logger/Logger.hpp"
#include <flatbuffers/flatbuffers.h>

namespace trade_bot {

std::vector<uint8_t> DashboardServer::serialize_state_binary_locked_() const {
    // Reuse builder across calls to avoid per-tick heap allocation.
    // Clear() resets state while keeping the backing buffer capacity.
    // A12: 128KB initial size to handle chart_history + density without realloc
    thread_local flatbuffers::FlatBufferBuilder builder(131072);
    builder.Clear();

    const auto& s = current_state_;

    // Account state
    auto account = TradeBotDashboard::CreateAccountState(
        builder,
        s.account.equity_usd,
        s.account.realized_pnl_today_usd,
        s.account.unrealized_pnl_usd,
        s.account.free_balance_usd,
        s.account.starting_equity_usd
    );

    // Signal counts (map → parallel arrays)
    std::vector<flatbuffers::Offset<flatbuffers::String>> signal_keys;
    std::vector<int32_t> signal_values;
    for (const auto& [k, v] : s.signal_counts) {
        signal_keys.push_back(builder.CreateString(k));
        signal_values.push_back(v);
    }
    auto signal_keys_vec   = builder.CreateVector(signal_keys);
    auto signal_values_vec = builder.CreateVector(signal_values);

    auto make_trade_plan = [&](const TradePlan& p) {
        return TradeBotDashboard::CreateTradePlan(
            builder,
            builder.CreateString(p.ticker),
            p.side == Side::Buy ? TradeBotDashboard::TradeSide_Buy : TradeBotDashboard::TradeSide_Sell,
            builder.CreateString(p.strategy_name.c_str(), p.strategy_name.size()),
            p.stop_price,
            p.tp1_price,
            p.size_coin,
            p.entry_price
        );
    };

    // Open trades
    std::vector<flatbuffers::Offset<TradeBotDashboard::OpenTrade>> open_trades_vec;
    for (const auto& t : s.open_trades) {
        open_trades_vec.push_back(TradeBotDashboard::CreateOpenTrade(
            builder, make_trade_plan(t.plan),
            t.executed_size, t.avg_entry_price, t.unrealized_pnl));
    }
    auto open_trades = builder.CreateVector(open_trades_vec);

    // Full-update-only offsets
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::JournalEntry>>>  recent_journal{};
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::StrategyStats>>> strategy_stats{};
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::FundingInfo>>>   funding_info{};
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::StrategyState>>> strategy_states{};
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::ChartPoint>>>    chart_history{};
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<TradeBotDashboard::DensityColumn>>> density_history{};
    flatbuffers::Offset<flatbuffers::Vector<const TradeBotDashboard::EquityPoint*>>                  equity_history{};

    // A1: Order book is now real-time, moved out of if (s.is_full_update)
    std::vector<TradeBotDashboard::BookLevel> bids_vec, asks_vec;
    bids_vec.reserve(s.bids_top20.size());
    asks_vec.reserve(s.asks_top20.size());
    for (const auto& lv : s.bids_top20) bids_vec.emplace_back(lv.price, lv.size);
    for (const auto& lv : s.asks_top20) asks_vec.emplace_back(lv.price, lv.size);
    auto bids_top20 = builder.CreateVectorOfStructs(bids_vec.data(), bids_vec.size());
    auto asks_top20 = builder.CreateVectorOfStructs(asks_vec.data(), asks_vec.size());

    if (s.is_full_update) {
        // Recent journal
        std::vector<flatbuffers::Offset<TradeBotDashboard::JournalEntry>> journal_vec;
        for (const auto& e : s.recent_journal) {
            journal_vec.push_back(TradeBotDashboard::CreateJournalEntry(
                builder, make_trade_plan(e.plan), e.pnl_usd, e.exit_price,
                builder.CreateString(e.cause_of_exit), e.ts_unix_ms));
        }
        recent_journal = builder.CreateVector(journal_vec);

        // Strategy stats
        std::vector<flatbuffers::Offset<TradeBotDashboard::StrategyStats>> stats_vec;
        for (const auto& st : s.strategy_stats) {
            stats_vec.push_back(TradeBotDashboard::CreateStrategyStats(
                builder, builder.CreateString(st.name),
                st.total_trades, st.wins, st.losses,
                st.total_pnl, st.best_pnl, st.worst_pnl,
                st.gross_profit, st.gross_loss));
        }
        strategy_stats = builder.CreateVector(stats_vec);

        // Funding info
        std::vector<flatbuffers::Offset<TradeBotDashboard::FundingInfo>> funding_vec;
        for (const auto& fi : s.funding_info) {
            funding_vec.push_back(TradeBotDashboard::CreateFundingInfo(
                builder, builder.CreateString(fi.ticker), fi.rate, fi.next_funding_unix));
        }
        funding_info = builder.CreateVector(funding_vec);

        // Strategy states
        std::vector<flatbuffers::Offset<TradeBotDashboard::StrategyState>> states_vec;
        for (const auto& ss : s.strategy_states) {
            std::vector<flatbuffers::Offset<TradeBotDashboard::StrategyCondition>> conds_vec;
            for (const auto& c : ss.conditions) {
                conds_vec.push_back(TradeBotDashboard::CreateStrategyCondition(
                    builder,
                    builder.CreateString(c.name),
                    c.current, c.target, c.met,
                    builder.CreateString(c.unit)));
            }
            // A13: Correct mapping of ReadyState
            TradeBotDashboard::ReadyState ready_state;
            switch (ss.ready_state) {
                case StrategyReadyState::Cold:     ready_state = TradeBotDashboard::ReadyState_NotReady; break;
                case StrategyReadyState::Warming:  ready_state = TradeBotDashboard::ReadyState_Warming;  break;
                case StrategyReadyState::Ready:    ready_state = TradeBotDashboard::ReadyState_Ready;    break;
                case StrategyReadyState::Planning: ready_state = TradeBotDashboard::ReadyState_Ready;    break;
                case StrategyReadyState::Trading:  ready_state = TradeBotDashboard::ReadyState_Ready;    break;
                case StrategyReadyState::Cooldown: ready_state = TradeBotDashboard::ReadyState_Ready;    break;
            }
            states_vec.push_back(TradeBotDashboard::CreateStrategyState(
                builder,
                builder.CreateString(ss.ticker),
                builder.CreateString(ss.strategy_name),
                ready_state,
                ss.readiness_pct,
                builder.CreateVector(conds_vec),
                builder.CreateString(ss.last_reject_reason),
                ss.seconds_since_last_reject,
                ss.signals_last_60s));
        }
        strategy_states = builder.CreateVector(states_vec);

        // Chart history
        std::vector<flatbuffers::Offset<TradeBotDashboard::ChartPoint>> chart_vec;
        for (const auto& pt : s.chart_history) {
            chart_vec.push_back(TradeBotDashboard::CreateChartPoint(
                builder,
                pt.ts_unix_ms, pt.mid, pt.best_bid, pt.best_ask,
                pt.spread_bps, pt.buy_vol_5s, pt.sell_vol_5s,
                pt.volatility_1min_bps, pt.tape_aggression,
                pt.leader_change_1s, pt.leader_correlation,
                pt.leader_lag_ms, pt.imbalance, pt.prints_per_sec));
        }
        chart_history = builder.CreateVector(chart_vec);

        // Density history (last 300 columns)
        std::vector<flatbuffers::Offset<TradeBotDashboard::DensityColumn>> density_vec;
        const auto& dh = s.density_history;
        constexpr std::size_t kMaxCols = 300;
        const std::size_t start = dh.size() > kMaxCols ? dh.size() - kMaxCols : 0;
        for (std::size_t k = start; k < dh.size(); ++k) {
            const auto& col = dh[k];
            density_vec.push_back(TradeBotDashboard::CreateDensityColumn(
                builder, col.ts_unix_ms, col.lo, col.hi,
                builder.CreateVector(col.bins.data(), col.bins.size())));
        }
        density_history = builder.CreateVector(density_vec);

        // Equity history — EquityPoint is a struct: up to 3600 × 16 bytes flat
        std::vector<TradeBotDashboard::EquityPoint> eq_vec;
        eq_vec.reserve(equity_history_.size());
        for (const auto& pt : equity_history_) eq_vec.emplace_back(pt.first, pt.second);
        equity_history = builder.CreateVectorOfStructs(eq_vec.data(), eq_vec.size());
    }

    // MetaScalp connection
    auto metascalp = TradeBotDashboard::CreateMetaScalpState(
        builder,
        s.metascalp.connected,
        static_cast<int32_t>(s.metascalp.latency_ms),
        builder.CreateString(s.metascalp.connection_name));

    // Universe rows — strategies is [string], no more CSV join
    std::vector<flatbuffers::Offset<TradeBotDashboard::UniverseRow>> universe_vec;
    for (const auto& r : s.universe_rows) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> strat_offsets;
        strat_offsets.reserve(r.strategies.size());
        for (const auto& st : r.strategies)
            strat_offsets.push_back(builder.CreateString(st));
        universe_vec.push_back(TradeBotDashboard::CreateUniverseRow(
            builder,
            builder.CreateString(r.ticker),
            builder.CreateVector(strat_offsets),
            r.boosted,
            r.mark_price));
    }
    auto universe_rows = builder.CreateVector(universe_vec);

    // Recent signals
    std::vector<flatbuffers::Offset<TradeBotDashboard::RecentSignal>> signals_vec;
    for (const auto& sg : s.recent_signals) {
        signals_vec.push_back(TradeBotDashboard::CreateRecentSignal(
            builder,
            builder.CreateString(sg.kind),
            builder.CreateString(sg.ticker),
            sg.price, sg.confidence,
            builder.CreateString(sg.time_str),
            builder.CreateString(sg.side)));
    }
    auto recent_signals = builder.CreateVector(signals_vec);

    // Risk metrics
    auto risk = TradeBotDashboard::CreateRiskMetrics(
        builder,
        s.risk.margin_used_pct,
        s.risk.exposure_pct,
        s.risk.total_trades_today,
        s.risk.consecutive_losses,
        s.risk.daily_pnl_pct,
        s.risk.current_drawdown_pct,
        s.risk.max_positions);

    // Iceberg events (always sent)
    std::vector<flatbuffers::Offset<TradeBotDashboard::IcebergEvent>> iceberg_vec;
    for (const auto& ev : s.iceberg_events) {
        iceberg_vec.push_back(TradeBotDashboard::CreateIcebergEvent(
            builder, ev.ts_ms, ev.price,
            builder.CreateString(ev.side), ev.hidden_size));
    }
    auto iceberg_events = builder.CreateVector(iceberg_vec);

    auto root = TradeBotDashboard::CreateDashboardState(
        builder,
        builder.CreateString(s.version),
        s.state_gen,
        s.is_full_update,
        s.server_time_unix,
        s.kill_switch_active,
        account,
        signal_keys_vec,
        signal_values_vec,
        open_trades,
        recent_journal,
        metascalp,
        universe_rows,
        recent_signals,
        strategy_stats,
        funding_info,
        risk,
        s.recorder_active,
        builder.CreateString(s.recorder_path),
        iceberg_events,
        strategy_states,
        chart_history,
        density_history,
        bids_top20,
        asks_top20,
        s.ob_mid,
        s.ob_spread_bps,
        s.ob_imbalance,
        builder.CreateString(s.selected_ticker),
        equity_history
    );

    builder.Finish(root);

    const uint8_t* buf = builder.GetBufferPointer();
    return std::vector<uint8_t>(buf, buf + builder.GetSize());
}

} // namespace trade_bot
