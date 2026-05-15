#include "numeric/Clustering.hpp"
#include <gtest/gtest.h>
#include <random>

using namespace trade_bot;

TEST(Dbscan1DTest, IdentifiesClustersAndNoise) {
    std::vector<double> values;
    
    // Cluster 1: around 100.0 (4 points)
    values.push_back(99.9);
    values.push_back(100.0);
    values.push_back(100.1);
    values.push_back(100.2);
    
    // Cluster 2: around 200.0 (3 points)
    values.push_back(199.5);
    values.push_back(200.0);
    values.push_back(200.5);
    
    // Noise points
    values.push_back(50.0);
    values.push_back(300.0);
    values.push_back(150.0);
    
    double eps = 1.0;
    size_t min_pts = 3;
    
    auto clusters = Dbscan1D::cluster(values, eps, min_pts);
    
    ASSERT_EQ(clusters.size(), 2);
    
    // Check sizes
    if (clusters[0].size() == 4) {
        EXPECT_EQ(clusters[1].size(), 3);
    } else {
        EXPECT_EQ(clusters[0].size(), 3);
        EXPECT_EQ(clusters[1].size(), 4);
    }
}

TEST(Dbscan1DTest, LargeSyntheticTest) {
    std::vector<double> values;
    std::mt19937_64 rng{42};
    std::normal_distribution<double> d1(100.0, 0.1);
    std::normal_distribution<double> d2(200.0, 0.1);
    std::normal_distribution<double> d3(300.0, 0.1);
    
    for (int i = 0; i < 30; ++i) values.push_back(d1(rng));
    for (int i = 0; i < 30; ++i) values.push_back(d2(rng));
    for (int i = 0; i < 30; ++i) values.push_back(d3(rng));
    
    // 10 noise points
    std::uniform_real_distribution<double> noise(0.0, 400.0);
    for (int i = 0; i < 10; ++i) values.push_back(noise(rng));
    
    auto clusters = Dbscan1D::cluster(values, 1.0, 5);
    EXPECT_EQ(clusters.size(), 3);
}
