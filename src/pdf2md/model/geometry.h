#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace pdf2md::model {

// Shared baseline defaults and coordinate sentinels across layout modules.
inline constexpr double kDefaultBodySizePt = 11.0;
inline constexpr double kMinFontSizePt = 1.0;
inline constexpr double kCoordSentinel = 1e30;
inline constexpr double kCoordSentinelThreshold = 1e29;
inline constexpr double kDefaultWordGapScale = 0.28;

// Axis-aligned page-space rectangle (origin bottom-left, y grows up).
struct Rect {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

constexpr bool rects_close(const Rect& a, const Rect& b, double gap) noexcept {
    return a.left - gap <= b.right && b.left - gap <= a.right &&
           a.bottom - gap <= b.top && b.bottom - gap <= a.top;
}

struct Interval {
    double lo = 0.0;
    double hi = 0.0;
};

// Merge sorted-by-lo intervals; return midpoints of gaps >= min_gap between them.
inline std::vector<double> find_gap_cuts(std::vector<Interval> ivals, double min_gap) {
    std::vector<double> cuts;
    if (ivals.empty()) {
        return cuts;
    }
    std::sort(ivals.begin(), ivals.end(), [](const Interval& a, const Interval& b) noexcept {
        if (a.lo != b.lo) {
            return a.lo < b.lo;
        }
        return a.hi < b.hi;
    });
    double cur_hi = ivals[0].hi;
    for (std::size_t i = 1U; i < ivals.size(); ++i) {
        if (ivals[i].lo > cur_hi + min_gap) {
            cuts.push_back((cur_hi + ivals[i].lo) / 2.0);
        }
        cur_hi = (std::max)(cur_hi, ivals[i].hi);
    }
    return cuts;
}

inline double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2U;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    double m = v[mid];
    if (v.size() % 2U == 0U) {
        const double lo =
            *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
        m = (m + lo) / 2.0;
    }
    return m;
}

// Thread-safe, warning-free check for an environment debug flag.
inline bool has_env_flag(const char* name) noexcept {
#ifdef _MSC_VER
    char* val = nullptr;
    std::size_t len = 0U;
    if (_dupenv_s(&val, &len, name) == 0 && val != nullptr) {
        std::free(val);
        return true;
    }
    return false;
#else
    return std::getenv(name) != nullptr;
#endif
}

}  // namespace pdf2md::model
