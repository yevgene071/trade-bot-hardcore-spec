#include "numeric/Hmm.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(HmmTest, PredictsCorrectStates) {
    ApproachHmm::Params p;
    p.start_probs = {1.0, 0.0, 0.0}; // Start at Impulse
    p.trans_matrix = {{
        {0.8, 0.2, 0.0},
        {0.1, 0.8, 0.1},
        {0.0, 0.1, 0.9}
    }};
    
    // Obs: [speed, pullbacks, dist]
    // 0: Impulse (high speed)
    p.emissions[0] = { .means = {10.0, 0.0, 100.0}, .stds = {1.0, 1.0, 10.0} };
    // 1: Slow (low speed)
    p.emissions[1] = { .means = {1.0, 5.0, 10.0}, .stds = {1.0, 1.0, 10.0} };
    // 2: Consolidation (zero speed)
    p.emissions[2] = { .means = {0.0, 10.0, 0.0}, .stds = {0.1, 1.0, 1.0} };
    
    ApproachHmm hmm(p);
    
    // Synthetic impulse trace
    std::vector<std::array<double, 3>> impulse_obs = {
        {11.0, 0.0, 95.0}, {10.0, 0.0, 105.0}
    };
    auto probs1 = hmm.predict(impulse_obs);
    EXPECT_GT(probs1[0], 0.8);
    
    // Synthetic slow trace
    std::vector<std::array<double, 3>> slow_obs = {
        {1.1, 4.5, 9.0}, {0.9, 5.5, 11.0}
    };
    auto probs2 = hmm.predict(slow_obs);
    EXPECT_GT(probs2[1], 0.8);
}
