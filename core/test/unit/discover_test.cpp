#include <gtest/gtest.h>
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/CurlHttpClient.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <thread>
#include <nlohmann/json.hpp>

using namespace trade_bot;
using namespace boost::asio;

class DiscoverTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/discover_test.log");
    }
};

void run_ping_server(int port, const std::string& app_name, const std::string& version) {
    try {
        io_context ioc;
        ip::tcp::acceptor acceptor(ioc, ip::tcp::endpoint(ip::tcp::v4(), port));
        ip::tcp::socket socket(ioc);
        acceptor.accept(socket);

        char data[1024];
        socket.read_some(buffer(data));

        nlohmann::json body;
        body["App"] = app_name;
        body["Version"] = version;
        std::string response_body = body.dump();

        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: " + std::to_string(response_body.length()) + "\r\n"
                               "Content-Type: application/json\r\n"
                               "\r\n" + response_body;
        write(socket, buffer(response));
    } catch (...) {}
}

TEST_F(DiscoverTest, SuccessDiscovery) {
    int port = 17850;
    std::thread server_thread(run_ping_server, port, "MetaScalp", "1.2.3");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto client = std::make_shared<CurlHttpClient>();
    MetaScalpDiscovery discovery(client);
    auto result = discovery.discover();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, port);

    server_thread.join();
}

TEST_F(DiscoverTest, DiscoveryTimeout) {
    auto client = std::make_shared<CurlHttpClient>();
    MetaScalpDiscovery discovery(client);
    
    // No server running on any port
    auto result = discovery.discover();

    EXPECT_FALSE(result.has_value());
}
