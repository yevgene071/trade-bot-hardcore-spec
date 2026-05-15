#include "CpuPinning.hpp"
#include "logger/Logger.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace trade_bot {

bool pin_thread_to_cpu(int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    pthread_t current_thread = pthread_self();
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        LOG_WARN("Failed to pin thread to CPU {}: error {}", cpu_id, result);
        return false;
    }
    LOG_DEBUG("Thread pinned to CPU {}", cpu_id);
    return true;
#elif defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu_id;
    DWORD_PTR result = SetThreadAffinityMask(thread, mask);
    if (result == 0) {
        LOG_WARN("Failed to pin thread to CPU {}: Windows error {}", cpu_id, GetLastError());
        return false;
    }
    LOG_DEBUG("Thread pinned to CPU {}", cpu_id);
    return true;
#else
    LOG_WARN("CPU pinning not supported on this platform");
    return false;
#endif
}

} // namespace trade_bot
