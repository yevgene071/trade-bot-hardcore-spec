#include "universe/ConnectionSelector.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

using namespace trade_bot;

namespace {

class ConnectionSelectorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

ConnectionInfo make(int id, const std::string& state, bool view_mode) {
    return ConnectionInfo{id, "conn-" + std::to_string(id), state, view_mode};
}

}  // namespace

TEST_F(ConnectionSelectorTest, RejectsViewMode) {
    ConnectionSelector sel;
    std::vector<ConnectionInfo> conns = {
        make(1, "Connected", /*view_mode=*/true),
    };
    EXPECT_EQ(sel.select(conns), std::nullopt);
}

TEST_F(ConnectionSelectorTest, RejectsNonConnectedState) {
    ConnectionSelector sel;
    std::vector<ConnectionInfo> conns = {
        make(1, "Disconnected", false),
        make(2, "Reconnecting", false),
    };
    EXPECT_EQ(sel.select(conns), std::nullopt);
}

TEST_F(ConnectionSelectorTest, ReturnsFirstEligible) {
    ConnectionSelector sel;
    std::vector<ConnectionInfo> conns = {
        make(1, "Disconnected", false),
        make(2, "Connected", true),         // view_mode → reject
        make(3, "Connected", false),        // first eligible
        make(4, "Connected", false),
    };
    EXPECT_EQ(sel.select(conns), 3);
}

TEST_F(ConnectionSelectorTest, EmptyListYieldsNullopt) {
    ConnectionSelector sel;
    EXPECT_EQ(sel.select({}), std::nullopt);
}
