#include "pdf2md/pdf/ocr_repair.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pdf2md::pdf {

namespace {

constexpr double kInflatedSizeRatio = 1.8;
constexpr double kDegenerateSizeRatio = 1.8;
constexpr double kOcrBaselineTolRatio = 0.45;
constexpr std::size_t kMinRepairGlyphs = 20U;

}  // namespace

void repair_ocr_text_layer(PageData& page_data) {
    std::size_t glyphs = 0U;
    std::size_t degenerate = 0U;
    for (const CharInfo& c : page_data.chars) {
        if (c.space || c.size <= 0.0) {
            continue;
        }
        ++glyphs;
        if (c.loose_height > kDegenerateSizeRatio * c.size) {
            ++degenerate;
        }
    }
    if (glyphs < kMinRepairGlyphs || degenerate * 2U <= glyphs) {
        return;
    }

    std::vector<CharInfo>& chars = page_data.chars;
    for (CharInfo& c : chars) {
        c.ocr = true;
        if (c.size > 0.0 && c.loose_height > kDegenerateSizeRatio * c.size) {
            c.size = c.loose_height;
        }
    }

    // Re-anchor each space between its stream neighbours while chars are still in stream order.
    for (std::size_t i = 1U; i + 1U < chars.size(); ++i) {
        CharInfo& sp = chars[i];
        if (!sp.space) {
            continue;
        }
        const CharInfo& prev = chars[i - 1U];
        const CharInfo& next = chars[i + 1U];
        if (prev.space || next.space) {
            continue;
        }
        const double tol = kOcrBaselineTolRatio * std::max({prev.size, next.size, 1.0});
        if (std::fabs(prev.y - next.y) > tol || next.left < prev.right - tol) {
            continue;
        }
        sp.left = prev.right;
        sp.right = (std::max)(next.left, prev.right);
        sp.x = sp.left;
        sp.y = prev.y;
    }
}

void repair_inflated_font_sizes(std::vector<PageData>& pages) {
    std::size_t total_chars = 0U;
    for (const PageData& page : pages) {
        total_chars += page.chars.size();
    }

    std::vector<double> healthy_ratios;
    healthy_ratios.reserve(total_chars);
    std::size_t glyphs = 0U;
    std::size_t inflated = 0U;
    for (const PageData& page : pages) {
        for (const CharInfo& c : page.chars) {
            if (c.space || c.size <= 0.0 || c.loose_height <= 0.0) {
                continue;
            }
            ++glyphs;
            if (c.size > kInflatedSizeRatio * c.loose_height) {
                ++inflated;
                continue;
            }
            healthy_ratios.push_back(c.size / c.loose_height);
        }
    }
    if (glyphs < kMinRepairGlyphs || inflated == 0U) {
        return;
    }

    double ratio = 1.0;
    if (!healthy_ratios.empty()) {
        const std::size_t mid = healthy_ratios.size() / 2U;
        std::nth_element(healthy_ratios.begin(),
                         healthy_ratios.begin() + static_cast<std::ptrdiff_t>(mid),
                         healthy_ratios.end());
        ratio = healthy_ratios[mid];
    }

    for (PageData& page : pages) {
        for (CharInfo& c : page.chars) {
            if (c.loose_height > 0.0 && c.size > kInflatedSizeRatio * c.loose_height) {
                c.size = c.loose_height * ratio;
            }
        }
    }
}

}  // namespace pdf2md::pdf
