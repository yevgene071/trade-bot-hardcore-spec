#include <gtest/gtest.h>
#include "control/KillSwitch.hpp"
#include "logger/Logger.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <csignal>

using namespace trade_bot;

class KillSwitchTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init();
        if (std::filesystem::exists("./killswitch")) {
            std::filesystem::remove("./killswitch");
        }
    }

    void TearDown() override {
        KillSwitch::instance().stop();
        if (std::filesystem::exists("./killswitch")) {
            std::filesystem::remove("./killswitch");
        }
    }
};

TEST_F(KillSwitchTest, ManualTrigger) {
    auto& ks = KillSwitch::instance();
    ks.start();
    
    EXPECT_FALSE(ks.is_triggered());
    ks.trigger(KillReason::Manual);
    EXPECT_TRUE(ks.is_triggered());
    EXPECT_TRUE(std::filesystem::exists("./killswitch"));
}

TEST_F(KillSwitchTest, FileTrigger) {
    auto& ks = KillSwitch::instance();
    ks.start();
    
    EXPECT_FALSE(ks.is_triggered());
    
    // Manually create the file
    {
        std::ofstream ofs("./killswitch");
        ofs << "test";
    }
    
    // Wait for watchdog
    bool triggered = false;
    for (int i = 0; i < 20; ++i) {
        if (ks.is_triggered()) {
            triggered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(triggered);
}

TEST_F(KillSwitchTest, SignalTrigger) {
    auto& ks = KillSwitch::instance();
    ks.start();
    
    EXPECT_FALSE(ks.is_triggered());
    
    std::raise(SIGINT);
    
    // Wait for watchdog
    bool triggered = false;
    for (int i = 0; i < 20; ++i) {
        if (ks.is_triggered()) {
            triggered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(triggered);
}
