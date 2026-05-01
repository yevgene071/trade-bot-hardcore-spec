#include <iostream>
#include <string>
#include <chrono>
#include <ctime>

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

int main() {
    std::cout << "--- Trade Bot Hardcore Edition ---\n";
    std::cout << "C++ Standard: 2023 (Target)\n";
    
    #ifdef _WIN32
        std::cout << "Platform: Windows\n";
    #else
        std::cout << "Platform: Linux\n";
    #endif

    bool avx2 = check_avx2();
    std::cout << "Status: Trade Bot Core is ready\n";
    std::cout << "AVX2 Optimization: " << (avx2 ? "Enabled" : "Disabled") << "\n";

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::cout << "System Time: " << std::ctime(&now_c);
    
    std::cout << "----------------------------------\n";
    
    return 0;
}
