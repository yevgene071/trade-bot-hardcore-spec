#include "numeric/Kde.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(KdeTest, IdentifiesLocalMaxima) {
    Kde::DataSet data;
    
    auto add = [&](double v, double w) {
        data.values.push_back(v);
        data.weights.push_back(w);
        data.total_weight += w;
    };

    // Peak 1: around 100.0
    add(99.0, 1.0);
    add(100.0, 5.0);
    add(101.0, 1.0);
    
    // Peak 2: around 200.0
    add(199.0, 1.0);
    add(200.0, 10.0);
    add(201.0, 1.0);
    
    std::vector<double> query = {100.0, 150.0, 200.0};
    double h = 2.0;
    
    auto densities = Kde::estimate(data, query, h);
    
    ASSERT_EQ(densities.size(), 3);
    EXPECT_GT(densities[0], densities[1]); // Peak 1 > Valley
    EXPECT_GT(densities[2], densities[1]); // Peak 2 > Valley
    EXPECT_GT(densities[2], densities[0]); // Peak 2 > Peak 1 (more weight)
}

TEST(KdeTest, SilvermanBandwidthIsReasonable) {
    Kde::DataSet data;
    data.values = {100.0, 101.0, 102.0};
    data.weights = {1.0, 1.0, 1.0};
    data.total_weight = 3.0;

    double h = Kde::silverman_bandwidth(data);
    EXPECT_GT(h, 0.1);
    EXPECT_LT(h, 5.0);
}
