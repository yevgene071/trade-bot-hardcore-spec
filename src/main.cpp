#include <iostream>
#include <print>
#include <expected>
#include <string>
#include <chrono>

#ifdef _WIN32
    #include <intrin.h>
#else
    #include <cpuid.h>
#endif

// Simple check for AVX2 support
bool check_avx2() {
#ifdef _WIN32
    int cpuInfo[4];
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
#else
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx & (1 << 5)) != 0;
    }
    return false;
#endif
}

std::expected<std::string, std::string> get_bot_status() {
    bool avx2 = check_avx2();
    if (!avx2) {
        return std::unexpected("Critical: AVX2 is not supported on this CPU!");
    }
    return "Trade Bot Core is ready (C++23)";
}

int main() {
    std::print("--- Trade Bot Hardcore Edition ---\n");
    std::print("C++ Standard: 2023\n");
    
    #ifdef _WIN32
        std::print("Platform: Windows\n");
    #else
        std::print("Platform: Linux\n");
    #endif

    auto status = get_bot_status();
    if (status) {
        std::print("Status: {}\n", *status);
        std::print("AVX2 Optimization: Enabled\n");
    } else {
        std::print("Status Error: {}\n", status.error());
        return 1;
    }

    std::print("System Time: {:%Y-%m-%d %H:%M:%S}\n", 
               std::chrono::system_clock::now());
    
    std::print("----------------------------------\n");
    
    return 0;
}
