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

} // namespace SimdOps
} // namespace trade_bot
