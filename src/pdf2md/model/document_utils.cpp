#include "pdf2md/model/document_utils.h"

#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace pdf2md::model {

namespace {

// Raised-origin detection (radicals / big math operators).
constexpr double kRaisedTopMarginScale = 0.30;
constexpr double kRaisedBottomDescentScale = 0.55;

constexpr int kBoldMajorityNumerator = 7;
constexpr int kBoldMajorityDenominator = 10;

}  // namespace

void compute_lines_geometry(std::span<const Line> lines, Block& b) {
    if (lines.empty()) {
        return;
    }
    double x0 = kCoordSentinel;
    double x1 = -kCoordSentinel;
    double y_top = -kCoordSentinel;
    double y_bottom = kCoordSentinel;
    std::vector<double> sizes;
    sizes.reserve(lines.size());
    int bold = 0;
    int total = 0;
    for (const Line& ln : lines) {
        if (ln.chars.empty()) {
            continue;  // synthetic blank line in code blocks
        }
        x0 = (std::min)(x0, ln.x0);
        x1 = (std::max)(x1, ln.x1);
        sizes.push_back(ln.size);
        for (const CharInfo& c : ln.chars) {
            if (c.space) {
                continue;
            }
            ++total;
            if (c.bold) {
                ++bold;
            }
            y_top = (std::max)(y_top, c.top);
            y_bottom = (std::min)(y_bottom, c.bottom);
        }
    }
    if (total == 0) {
        b.x0 = 0.0;
        b.x1 = 0.0;
        b.y_top = 0.0;
        b.y_bottom = 0.0;
        b.size = kMinFontSizePt;
        b.bold_majority = false;
        return;
    }
    b.x0 = x0;
    b.x1 = x1;
    b.y_top = y_top;
    b.y_bottom = y_bottom;
    b.size = (std::max)(median(std::move(sizes)), kMinFontSizePt);
    b.bold_majority = bold * kBoldMajorityDenominator >= total * kBoldMajorityNumerator;
}

void finalize_block_geometry(Block& b) {
    compute_lines_geometry(b.lines, b);
}

std::size_t count_glyphs(const Line& line) noexcept {
    std::size_t n = 0U;
    for (const CharInfo& c : line.chars) {
        if (!c.space) {
            ++n;
        }
    }
    return n;
}

// A tall math symbol (a radical or a big operator like the summation) reports
// its origin near the TOP of its box, with the box descending well below that
// origin. Placing it by its baseline would cluster it with the line above and
// scatter its radicand/limits, so it is located by its box centre instead.
bool raised_origin_glyph(const CharInfo& c) noexcept {
    if (c.size <= 0.0 || c.top <= c.bottom) {
        return false;
    }
    return (c.top - c.y) < kRaisedTopMarginScale * c.size &&
           (c.y - c.bottom) > kRaisedBottomDescentScale * c.size;
}

bool is_citation_lead(const Line& ln) noexcept {
    const std::u32string& p = ln.plain;
    std::size_t i = 0U;
    while (i < p.size() && p[i] == U' ') {
        ++i;
    }
    if (i >= p.size() || p[i] != U'[') {
        return false;
    }
    const std::size_t d = ++i;
    while (i < p.size() && is_ascii_digit(p[i])) {
        ++i;
    }
    return i > d && i < p.size() && p[i] == U']';
}

}  // namespace pdf2md::model
