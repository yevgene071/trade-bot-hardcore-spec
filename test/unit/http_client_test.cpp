#include <gtest/gtest.h>
#include "transport/CurlHttpClient.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <thread>

using namespace trade_bot;
using namespace boost::asio;

class HttpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/http_test.log");
    }
};

// Simple mock server using boost::asio
void run_mock_server(int port, const std::string& response_body, int status_code = 200) {
    io_context ioc;
    ip::tcp::acceptor acceptor(ioc, ip::tcp::endpoint(ip::tcp::v4(), port));
    ip::tcp::socket socket(ioc);
    acceptor.accept(socket);

    char data[1024];
    socket.read_some(buffer(data));

    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n"
                           "Content-Length: " + std::to_string(response_body.length()) + "\r\n"
                           "Content-Type: text/plain\r\n"
                           "\r\n" + response_body;
    write(socket, buffer(response));
}

TEST_F(HttpClientTest, GetRequest) {
    int port = 18888;
    std::string expected_body = "Hello World";
    
    std::thread server_thread(run_mock_server, port, expected_body, 200);
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CurlHttpClient client;
    auto response = client.get("http://127.0.0.1:" + std::to_string(port));

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, expected_body);

    server_thread.join();
}

TEST_F(HttpClientTest, PostRequest) {
    int port = 18889;
    std::string expected_body = "Created";
    
    std::thread server_thread(run_mock_server, port, expected_body, 201);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CurlHttpClient client;
    auto response = client.post("http://127.0.0.1:" + std::to_string(port), "some data");

    EXPECT_EQ(response.status, 201);
    EXPECT_EQ(response.body, expected_body);

    server_thread.join();
}

TEST_F(HttpClientTest, Timeout) {
    CurlHttpClient client;
    client.set_timeout_ms(100);
    
    // Try to connect to a non-responsive port
    auto response = client.get("http://127.0.0.1:19999");
    
    EXPECT_EQ(response.status, -1);
}
