#pragma once

#include <cstdint>
#include <compare>
#include <cmath>
#include <stdexcept>

namespace trade_bot {

/**
 * Strong type for price ticks using int64_t to avoid floating point issues.
 */
struct PriceTick {
    int64_t ticks;

    static PriceTick from_price(double price, double increment) {
        if (!std::isfinite(price) || price <= 0) return {0};
        // T4-MATH: Prevent overflow on extreme prices (#144)
        double t = std::round(price / increment);
        return { static_cast<int64_t>(std::clamp(t, 0.0, 1e15)) };
    }

    static PriceTick from_price_inv(double price, double inv_increment) {
        if (!std::isfinite(price) || price <= 0) return {0};
        // B9-FIX: Check for potential overflow BEFORE multiplication
        constexpr double kMaxInt64 = 9.223372036854775807e18;
        if (price * inv_increment > kMaxInt64) {
            return { static_cast<int64_t>(1e15) };  // Return clamped max
        }
        double t = std::round(price * inv_increment);
        return { static_cast<int64_t>(std::clamp(t, 0.0, 1e15)) };
    }

    double to_price(double increment) const {
        return static_cast<double>(ticks) * increment;
    }

    auto operator<=>(const PriceTick&) const = default;

    template <typename H>
    friend H AbslHashValue(H h, const PriceTick& p) {
        return H::combine(std::move(h), p.ticks);
    }

    PriceTick operator+(const PriceTick& other) const { return { ticks + other.ticks }; }
    PriceTick operator-(const PriceTick& other) const { return { ticks - other.ticks }; }
};

/**
 * Strong type for sizes (volume) using int64_t to avoid floating point issues.
 * Typically 1 unit = 0.00000001 (satoshis) or similar depending on the asset.
 */
struct SizeFix {
    int64_t raw;

    static SizeFix from_double(double size, double increment = 1e-8) {
        if (!std::isfinite(size) || size <= 0) return {0};
        // T4-MATH: Prevent overflow on extreme sizes (#144)
        double t = std::round(size / increment);
        return { static_cast<int64_t>(std::clamp(t, 0.0, 1e15)) };
    }

    static SizeFix from_double_inv(double size, double inv_increment) {
        if (!std::isfinite(size) || size <= 0) return {0};
        double t = std::round(size * inv_increment);
        return { static_cast<int64_t>(std::clamp(t, 0.0, 1e15)) };
    }

    double to_double(double increment = 1e-8) const {
        return static_cast<double>(raw) * increment;
    }

    auto operator<=>(const SizeFix&) const = default;
};

} // namespace trade_bot
