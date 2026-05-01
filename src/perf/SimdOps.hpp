#pragma once

#include <cstddef>

namespace trade_bot {

/**
 * SIMD-accelerated operations using xsimd.
 */
namespace SimdOps {

double sum(const double* data, size_t n);
double dot_product(const double* a, const double* b, size_t n);

} // namespace SimdOps

} // namespace trade_bot
