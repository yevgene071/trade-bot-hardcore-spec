#include "CurlHttpClient.hpp"
#include "logger/Logger.hpp"
#include <algorithm>
#include <cstring>

namespace trade_bot {

CurlHttpClient::CurlHttpClient() {
    curl_global_init(CURL_GLOBAL_ALL);
}

CurlHttpClient::~CurlHttpClient() {
    curl_global_cleanup();
}

void CurlHttpClient::set_timeout_ms(int timeout_ms) {
    m_timeout_ms = timeout_ms;
}

HttpResponse CurlHttpClient::get(const std::string& url) {
    return perform_request(url, "GET");
}

HttpResponse CurlHttpClient::post(const std::string& url, const std::string& body) {
    return perform_request(url, "POST", body);
}

HttpResponse CurlHttpClient::put(const std::string& url, const std::string& body) {
    return perform_request(url, "PUT", body);
}

HttpResponse CurlHttpClient::del(const std::string& url) {
    return perform_request(url, "DELETE");
}

size_t CurlHttpClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t CurlHttpClient::header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(buffer, size * nitems);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        
        // Trim whitespace
        name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), name.end());
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
        
        (*headers)[name] = value;
    }
    return size * nitems;
}

HttpResponse CurlHttpClient::perform_request(const std::string& url, const std::string& method, const std::string& body) {
    CURL* curl = curl_easy_init();
    HttpResponse response{0, "", {}};
    
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)m_timeout_ms);

        // Required for multithreading safety in some environments
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            LOG_ERROR("CURL request failed: {} (URL: {})", curl_easy_strerror(res), url);
            response.status = -1;
            response.body = curl_easy_strerror(res);
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            response.status = (int)http_code;
        }

        curl_easy_cleanup(curl);
    } else {
        LOG_ERROR("Failed to initialize CURL for URL: {}", url);
        response.status = -1;
    }

    return response;
}

} // namespace trade_bot
