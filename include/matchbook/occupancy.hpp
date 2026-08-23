#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace matchbook {

// Three-level occupancy bitmap over a fixed band of ticks. Each level records
// which words of the level below it are non-empty, so the best occupied tick
// costs three dependent loads and three bit scans whatever the band width.
class Occupancy {
public:
    static constexpr std::size_t npos = ~std::size_t{0};
    static constexpr std::size_t max_ticks = 64 * 64 * 64;

    explicit Occupancy(std::size_t ticks)
        : l0_(words(ticks)), l1_(words(words(ticks))), l2_(1, 0) {}

    void set(std::size_t i) {
        const std::size_t w = i / 64, v = w / 64;
        l0_[w] |= mask(i);
        l1_[v] |= mask(w);
        l2_[0] |= mask(v);
    }

    void clear(std::size_t i) {
        const std::size_t w = i / 64, v = w / 64;
        l0_[w] &= ~mask(i);
        if (l0_[w] != 0) return;
        l1_[v] &= ~mask(w);
        if (l1_[v] != 0) return;
        l2_[0] &= ~mask(v);
    }

    [[nodiscard]] bool test(std::size_t i) const { return (l0_[i / 64] & mask(i)) != 0; }
    [[nodiscard]] bool empty() const { return l2_[0] == 0; }

    [[nodiscard]] std::size_t lowest() const {
        if (empty()) return npos;
        const std::size_t v = std::countr_zero(l2_[0]);
        const std::size_t w = v * 64 + std::countr_zero(l1_[v]);
        return w * 64 + std::countr_zero(l0_[w]);
    }

    [[nodiscard]] std::size_t highest() const {
        if (empty()) return npos;
        const std::size_t v = top(l2_[0]);
        const std::size_t w = v * 64 + top(l1_[v]);
        return w * 64 + top(l0_[w]);
    }

    [[nodiscard]] std::size_t next_above(std::size_t i) const {
        const std::size_t w = i / 64, v = w / 64;
        if (const std::uint64_t rest = l0_[w] & above(i)) return w * 64 + std::countr_zero(rest);
        if (const std::uint64_t rest = l1_[v] & above(w)) {
            const std::size_t w2 = v * 64 + std::countr_zero(rest);
            return w2 * 64 + std::countr_zero(l0_[w2]);
        }
        const std::uint64_t rest = l2_[0] & above(v);
        if (rest == 0) return npos;
        const std::size_t v2 = std::countr_zero(rest);
        const std::size_t w2 = v2 * 64 + std::countr_zero(l1_[v2]);
        return w2 * 64 + std::countr_zero(l0_[w2]);
    }

    [[nodiscard]] std::size_t next_below(std::size_t i) const {
        const std::size_t w = i / 64, v = w / 64;
        if (const std::uint64_t rest = l0_[w] & below(i)) return w * 64 + top(rest);
        if (const std::uint64_t rest = l1_[v] & below(w)) {
            const std::size_t w2 = v * 64 + top(rest);
            return w2 * 64 + top(l0_[w2]);
        }
        const std::uint64_t rest = l2_[0] & below(v);
        if (rest == 0) return npos;
        const std::size_t v2 = top(rest);
        const std::size_t w2 = v2 * 64 + top(l1_[v2]);
        return w2 * 64 + top(l0_[w2]);
    }

private:
    static std::size_t words(std::size_t n) { return (n + 63) / 64; }
    static std::uint64_t mask(std::size_t i) { return std::uint64_t{1} << (i % 64); }
    static std::size_t top(std::uint64_t word) { return std::bit_width(word) - 1; }

    // Shifting by 64 is undefined, so the saturated ends are spelled out.
    static std::uint64_t above(std::size_t i) {
        return i % 64 == 63 ? 0 : ~std::uint64_t{0} << (i % 64 + 1);
    }
    static std::uint64_t below(std::size_t i) {
        return i % 64 == 0 ? 0 : ~std::uint64_t{0} >> (64 - i % 64);
    }

    std::vector<std::uint64_t> l0_, l1_, l2_;
};

}  // namespace matchbook
