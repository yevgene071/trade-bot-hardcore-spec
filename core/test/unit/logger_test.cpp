#include <gtest/gtest.h>
#include "logger/Logger.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (fs::exists("test_logs")) {
            fs::remove_all("test_logs");
        }
        fs::create_directory("test_logs");
    }

    void TearDown() override {
        // We can't easily shut down spdlog and restart it in the same process
        // but we can at least clean up files afterwards if possible.
    }
};

TEST_F(LoggerTest, FileFormatVerification) {
    std::string test_log = "test_logs/test.log";
    trade_bot::Logger::init(test_log);
    
    LOG_INFO("Verification message");
    
    // Flush to ensure it's written
    trade_bot::Logger::get()->flush();
    
    // Find the actual file (spdlog might append date if it's a daily sink)
    std::string actual_file = test_log;
    // For daily sink, the file name will be test.YYYY-MM-DD.log
    // but since we passed 0, 0 (midnight), it should be just test.log initially or similar.
    // Let's look for any .log in test_logs
    for (const auto& entry : fs::directory_iterator("test_logs")) {
        actual_file = entry.path().string();
    }

    std::ifstream file(actual_file);
    ASSERT_TRUE(file.is_open());
    
    std::string line;
    std::getline(file, line);
    
    // Format: [YYYY-MM-DDTHH:MM:SS.sssZ] [info] [source:line] Verification message
    EXPECT_TRUE(line.find("[info]") != std::string::npos);
    EXPECT_TRUE(line.find("Verification message") != std::string::npos);
    // Check for ISO-ish date bracket
    EXPECT_EQ(line[0], '[');
}
