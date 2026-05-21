#include "CurlHttpClient.hpp"
#include "logger/Logger.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace trade_bot {

namespace {
// O1: curl_global_init/cleanup must happen exactly once per process.
void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        curl_global_init(CURL_GLOBAL_ALL);
        std::atexit(curl_global_cleanup);
    });
}
} // namespace

CurlHttpClient::CurlHttpClient() {
    ensure_curl_global_init();
    m_curl = curl_easy_init();  // O2: persistent handle for connection keep-alive
}

CurlHttpClient::~CurlHttpClient() {
    if (m_curl) {
        curl_easy_cleanup(m_curl);
    }
}

void CurlHttpClient::set_timeout_ms(int timeout_ms) {
    m_timeout_ms.store(timeout_ms, std::memory_order_relaxed);
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

        auto ltrim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
        };
        auto rtrim = [](std::string& s) {
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
        };
        ltrim(name); rtrim(name);
        ltrim(value); rtrim(value);

        // O6: normalize header name to lowercase for case-insensitive lookup
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        (*headers)[name] = value;
    }
    return size * nitems;
}

HttpResponse CurlHttpClient::perform_request(const std::string& url, const std::string& method, const std::string& body) {
    std::lock_guard<std::mutex> lk(m_mtx);  // O2: protect reused handle
    HttpResponse response{0, "", {}};

    if (!m_curl) {
        LOG_ERROR("CURL handle not initialized (method: {})", method);
        response.status = -1;
        return response;
    }

    curl_easy_reset(m_curl);  // O2: clear state, keep internal connection cache for keep-alive

    const int timeout = m_timeout_ms.load(std::memory_order_relaxed);

    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
    curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout / 3));  // O3

    struct curl_slist* req_headers = nullptr;
    if (!body.empty()) {
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        // O4: set Content-Type for requests with a body
        req_headers = curl_slist_append(req_headers, "Content-Type: application/json");
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, req_headers);
    }

    curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(m_curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(m_curl);

    if (req_headers) {
        curl_slist_free_all(req_headers);
    }

    if (res != CURLE_OK) {
        // O5: do not log full URL — query params may contain auth tokens
        LOG_ERROR("CURL {} failed: {}", method, curl_easy_strerror(res));
        response.status = -1;
        response.body   = curl_easy_strerror(res);
    } else {
        long http_code = 0;
        curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status = static_cast<int>(http_code);
    }

    return response;
}

} // namespace trade_bot
