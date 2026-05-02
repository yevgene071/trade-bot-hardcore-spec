#include "SimdOps.hpp"
#include <xsimd/xsimd.hpp>
#include <algorithm>

namespace trade_bot {
namespace SimdOps {

double sum(const double* data, size_t n) {
    using batch_type = xsimd::batch<double>;
    constexpr size_t step = batch_type::size;
    
    batch_type acc = batch_type(0.0);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        auto b = batch_type::load_unaligned(data + i);
        acc += b;
    }
    
    double total = xsimd::reduce_add(acc);
    for (; i < n; ++i) {
        total += data[i];
    }
    return total;
}

double dot_product(const double* a, const double* b, size_t n) {
    using batch_type = xsimd::batch<double>;
    constexpr size_t step = batch_type::size;
    
    batch_type acc = batch_type(0.0);
    size_t i = 0;
    for (; i + step <= n; i += step) {
        auto ba = batch_type::load_unaligned(a + i);
        auto bb = batch_type::load_unaligned(b + i);
        acc = xsimd::fma(ba, bb, acc);
    }
    
    double total = xsimd::reduce_add(acc);
    for (; i < n; ++i) {
        total += a[i] * b[i];
    }
    return total;
}

void prefix_sum(const double* data, double* out, size_t n) {
    if (n == 0) return;
    double running = 0.0;
    for (size_t i = 0; i < n; ++i) {
        running += data[i];
        out[i] = running;
    }
    // Note: SIMD prefix sum is complex to implement efficiently for small n, 
    // using scalar for now as baseline. ARCH mentions SIMD, but for n=10-30 
    // scalar is often faster due to dependency chain.
}

} // namespace SimdOps
} // namespace trade_bot
