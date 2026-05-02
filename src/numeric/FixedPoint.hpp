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
        return { static_cast<int64_t>(std::round(price / increment)) };
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
        return { static_cast<int64_t>(std::round(size / increment)) };
    }

    double to_double(double increment = 1e-8) const {
        return static_cast<double>(raw) * increment;
    }

    auto operator<=>(const SizeFix&) const = default;
};

} // namespace trade_bot
