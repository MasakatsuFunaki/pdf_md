#include "layout_internal.h"

#include "utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdf2md {
namespace detail {

namespace {

// Special Unicode markers used across document-level passes.
constexpr char32_t kSoftHyphenCp = 0x00AD;
constexpr char32_t kUnicodeHyphenCp = 0x2010;
constexpr char32_t kContAboveTriangleCp = 0x25B3;
constexpr char32_t kContBelowTriangleCp = 0x25BD;
constexpr char32_t kAsciiLimitCp = 0x80;

// Body-size histogram quantization (quarter-point buckets).
constexpr double kQuarterPtScale = 4.0;
constexpr size_t kFastHistBuckets = 512;

// Running-chrome and watermark detection constants.
constexpr double kHeaderBandBottomRatio = 0.88;
constexpr double kFooterBandTopRatio = 0.12;
constexpr double kChromeQuantizeBinPt = 4.0;
constexpr double kDefaultIsolationDistPt = 24.0;
constexpr double kMarginLeftRatio = 0.15;
constexpr double kMarginRightRatio = 0.85;
constexpr size_t kMinPagesForLineChrome = 3;
constexpr size_t kMinPagesForBlockChrome = 2;
constexpr size_t kMinChromeRepeatCount = 3;
constexpr size_t kPosBandThreshold = 4;
constexpr size_t kWatermarkMinSigLen = 2;
constexpr size_t kWatermarkMaxSigLen = 24;
constexpr double kPosChromeIsolationLineScale = 1.8;
constexpr double kPosChromeMinIsolationDistPt = 14.0;

// Heading classification constants (derived relative to body_size).
constexpr double kHalfPtScale = 2.0;
constexpr size_t kHeadingMaxLines = 3;
constexpr size_t kHeadingMaxChars = 220;
constexpr double kHeadingMinSizeScale = 1.12;
constexpr size_t kHeadingMaxDocCharPercent = 30;
constexpr size_t kPercentScale = 100;
constexpr int kMaxHeadingLevel = 6;
constexpr int kMinBoldHeadingLevel = 2;
constexpr size_t kBoldHeadingMaxChars = 90;
constexpr double kBoldHeadingMinSizeScale = 0.98;
constexpr size_t kMaxMetaLabelLen = 40;
constexpr int kRepeatedHeadingMinCount = 3;
constexpr double kInsetCalloutMinRatio = 0.02;
constexpr size_t kRunInHeadingMaxChars = 140;

// Cross-block and cross-page stitching constants.
constexpr double kLoneMathSizeDiffRatio = 0.20;
constexpr double kTableSigMinTolPt = 3.0;
constexpr double kTableSigTolScale = 0.4;
constexpr double kTocAlignMinTolPt = 3.0;
constexpr double kTocAlignTolScale = 0.5;
constexpr double kTocMaxVGapScale = 2.5;
constexpr double kRefEntrySizeDiffRatio = 0.15;
constexpr double kCrossPageSizeDiffRatio = 0.08;
constexpr size_t kPipelinePassCount = 9;

// Thread-safe, warning-free check for an environment debug flag.
bool has_env_flag(const char* name) noexcept {
#ifdef _MSC_VER
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, name) == 0 && val != nullptr) {
        std::free(val);
        return true;
    }
    return false;
#else
    return std::getenv(name) != nullptr;
#endif
}

constexpr char ascii_tolower(char32_t cp) noexcept {
    if (cp >= U'A' && cp <= U'Z') {
        return static_cast<char>(cp - U'A' + U'a');
    }
    return static_cast<char>(cp);
}

std::string normalized_block_text(const Block& b) {
    std::string out;
    for (const Line& ln : b.lines) {
        for (char32_t cp : ln.plain) {
            if (is_ascii_digit(cp)) {
                if (out.empty() || out.back() != '#') {
                    out += '#';
                }
            } else if (cp == U' ') {
                continue;
            } else if (cp < kAsciiLimitCp) {
                out += ascii_tolower(cp);
            } else {
                append_utf8(out, cp);
            }
        }
        out += '|';
    }
    return out;
}

constexpr bool is_page_number_text(std::string_view norm) noexcept {
    constexpr std::array<std::string_view, 7> kPatterns = {
        "#|", "-#-|", "page#|", "page#of#|", "#/#|", "[#]|", "|"};
    for (std::string_view p : kPatterns) {
        if (norm == p) {
            return true;
        }
    }
    return false;
}

// Digit runs collapse to '#', spaces drop, ASCII lowercases.
std::string chrome_segment_sig(const Line& ln, size_t first, size_t last) {
    std::string out;
    for (size_t i = first; i <= last && i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        const char32_t cp = c.code;
        if (is_ascii_digit(cp)) {
            if (out.empty() || out.back() != '#') {
                out += '#';
            }
        } else if (cp < kAsciiLimitCp) {
            out += ascii_tolower(cp);
        } else {
            append_utf8(out, cp);
        }
    }
    return out;
}

// 1 = header band (top margin), 2 = footer band, 0 = body (PDF space, y grows up).
int chrome_band_of(const Line& ln, double h) noexcept {
    if (h <= 0 || ln.chars.empty()) {
        return 0;
    }
    double top = -kCoordSentinel;
    double bottom = kCoordSentinel;
    for (const CharInfo& c : ln.chars) {
        top = std::max(top, c.top);
        bottom = std::min(bottom, c.bottom);
    }
    if (bottom > kHeaderBandBottomRatio * h) {
        return 1;
    }
    if (top < kFooterBandTopRatio * h) {
        return 2;
    }
    return 0;
}

// Returns a non-allocating view over `ln.segments`, falling back to `fallback`
// covering `[0, ln.chars.size() - 1]` when `ln.segments` is empty.
std::span<const std::pair<size_t, size_t>> chrome_segments_of(
    const Line& ln, std::pair<size_t, size_t>& fallback) noexcept {
    if (!ln.segments.empty()) {
        return ln.segments;
    }
    if (!ln.chars.empty()) {
        fallback = {0, ln.chars.size() - 1};
        return std::span<const std::pair<size_t, size_t>>(&fallback, 1);
    }
    return {};
}

std::pair<long, long> chrome_position_key(const Line& ln, size_t first) noexcept {
    if (first >= ln.chars.size()) {
        return {0, 0};
    }
    return {std::lround(ln.chars[first].left / kChromeQuantizeBinPt),
            std::lround(ln.baseline / kChromeQuantizeBinPt)};
}

std::string chrome_watermark_sig(const Line& ln, size_t first, size_t last) {
    std::string out;
    for (size_t i = first; i <= last && i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) {
            continue;
        }
        const char32_t cp = c.code;
        if (cp < kAsciiLimitCp) {
            out += ascii_tolower(cp);
        } else {
            append_utf8(out, cp);
        }
    }
    return out;
}

bool chrome_isolated_span(double x0, double x1, double baseline, const Line& self,
                          const PageLines& pg,
                          double dist = kDefaultIsolationDistPt) noexcept {
    for (const auto& region : pg.regions) {
        for (const Line& other : region) {
            if (&other == &self) {
                continue;
            }
            if (other.x0 <= x1 && x0 <= other.x1 &&
                std::fabs(other.baseline - baseline) <= dist) {
                return false;
            }
        }
    }
    return true;
}

bool chrome_in_margin(const Line& ln, size_t first, size_t last, double page_w) noexcept {
    if (page_w <= 0 || ln.chars.empty() || first >= ln.chars.size()) {
        return false;
    }
    const double x0 = ln.chars[first].left;
    const double x1 = ln.chars[std::min(last, ln.chars.size() - 1)].right;
    return x1 < kMarginLeftRatio * page_w || x0 > kMarginRightRatio * page_w;
}

void strip_headers_footers(std::vector<Block>& blocks, const std::vector<PageData>& pages) {
    if (pages.size() < kMinPagesForBlockChrome) {
        return;
    }
    std::unordered_map<int, std::pair<double, double>> page_dims;
    page_dims.reserve(pages.size());
    for (const PageData& p : pages) {
        page_dims.insert_or_assign(p.index, std::make_pair(p.width, p.height));
    }

    auto zone_of = [&page_dims](const Block& b) -> int {
        const auto it = page_dims.find(b.page);
        if (it == page_dims.end()) {
            return 0;
        }
        const double h = it->second.second;
        if (h <= 0) {
            return 0;
        }
        if (b.y_bottom > kHeaderBandBottomRatio * h) {
            return 1;
        }
        if (b.y_top < kFooterBandTopRatio * h) {
            return 2;
        }
        return 0;
    };

    std::map<std::pair<int, std::string>, std::map<int, int>> seen;
    for (const Block& b : blocks) {
        if (b.kind == BlockKind::Image) {
            continue;
        }
        const int z = zone_of(b);
        if (z == 0) {
            continue;
        }
        seen[{z, normalized_block_text(b)}][b.page]++;
    }

    const size_t repeat_threshold =
        std::max<size_t>(kMinChromeRepeatCount, (pages.size() + 1) / 2);

    const auto& const_seen = seen;
    std::erase_if(blocks, [&zone_of, &const_seen, repeat_threshold](const Block& b) {
        if (b.kind == BlockKind::Image) {
            return false;
        }
        const int z = zone_of(b);
        if (z == 0) {
            return false;
        }
        const std::string norm = normalized_block_text(b);
        if (is_page_number_text(norm)) {
            return true;
        }
        const auto it = const_seen.find({z, norm});
        return it != const_seen.end() && it->second.size() >= repeat_threshold;
    });
}

int heading_number_depth(const Block& b) noexcept {
    if (b.lines.empty()) {
        return 0;
    }
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    if (i >= s.size() || !is_ascii_digit(s[i])) {
        return 0;
    }
    int depth = 0;
    while (i < s.size() && is_ascii_digit(s[i])) {
        while (i < s.size() && is_ascii_digit(s[i])) {
            ++i;
        }
        ++depth;
        if (i + 1 < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1])) {
            ++i;
        } else {
            break;
        }
    }
    if (i < s.size() && s[i] != U' ' && s[i] != U'.') {
        return 0;
    }
    return depth;
}

struct SectionNumber {
    std::u32string canon;
    int depth = 0;
};

SectionNumber parse_section_number(std::u32string_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    std::u32string canon;
    int depth = 0;
    if (i + 2 < s.size() && is_ascii_upper(s[i]) && s[i + 1] == U'.' &&
        is_ascii_digit(s[i + 2])) {
        canon += s[i];
        ++depth;
        i += 2;
    }
    bool have_number = false;
    while (i < s.size() && is_ascii_digit(s[i])) {
        if (!canon.empty()) {
            canon += U'.';
        }
        while (i < s.size() && is_ascii_digit(s[i])) {
            canon += s[i];
            ++i;
        }
        ++depth;
        have_number = true;
        if (i + 1 < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1])) {
            ++i;
        } else {
            break;
        }
    }
    if (!have_number || i >= s.size() || s[i] != U' ') {
        return {};
    }
    size_t t = i;
    while (t < s.size() && s[t] == U' ') {
        ++t;
    }
    if (t >= s.size()) {
        return {};
    }
    return {std::move(canon), depth};
}

std::string heading_norm_key(const Block& b) {
    std::string k;
    bool gap = false;
    for (const Line& ln : b.lines) {
        for (char32_t c : ln.plain) {
            if (is_ascii_upper(c) || is_ascii_lower(c) || is_ascii_digit(c)) {
                if (gap && !k.empty()) {
                    k += ' ';
                }
                k += ascii_tolower(c);
                gap = false;
            } else {
                gap = true;
            }
        }
        gap = true;
    }
    return k;
}

std::string heading_meta_label(const Block& b) {
    if (b.lines.empty()) {
        return {};
    }
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    std::string label;
    bool has_letter = false;
    for (; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (c == U':') {
            if (i + 1 < s.size() && s[i + 1] != U' ') {
                return {};
            }
            break;
        }
        if (is_letter_cp(c)) {
            has_letter = true;
        } else if (!is_ascii_digit(c) && c != U' ' && c != U'-' && c != U'_' && c != U'/' &&
                   c != U'(' && c != U')' && c != U'&' && c != U',') {
            return {};
        }
        if (c < kAsciiLimitCp) {
            label += static_cast<char>(c);
        }
        if (label.size() > kMaxMetaLabelLen) {
            return {};
        }
    }
    if (i >= s.size() || !has_letter) {
        return {};
    }
    while (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    return label;
}

bool heading_is_caption(const Block& b) noexcept {
    if (b.lines.empty()) {
        return false;
    }
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') {
        ++i;
    }
    auto matches = [&s, i](std::u32string_view w) {
        if (s.size() < i + w.size() + 2) {
            return false;
        }
        if (std::u32string_view(s).substr(i, w.size()) != w) {
            return false;
        }
        const size_t k = i + w.size();
        return s[k] == U' ' && is_ascii_digit(s[k + 1]);
    };
    return matches(U"Figure") || matches(U"Table");
}

void classify_headings(std::vector<Block>& blocks, double body_size) {
    size_t total_chars = 0;
    for (const Block& b : blocks) {
        for (const Line& ln : b.lines) {
            total_chars += ln.plain.size();
        }
    }

    auto tier_key = [](double size) { return std::lround(size * kHalfPtScale); };

    std::map<long, size_t, std::greater<long>> tier_chars;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || b.lines.size() > kHeadingMaxLines) {
            continue;
        }
        size_t len = 0;
        for (const Line& ln : b.lines) {
            len += ln.plain.size();
        }
        if (len == 0 || len > kHeadingMaxChars) {
            continue;
        }
        if (b.size >= body_size * kHeadingMinSizeScale) {
            tier_chars[tier_key(b.size)] += len;
        }
    }

    std::vector<long> tiers;
    tiers.reserve(tier_chars.size());
    for (const auto& [key, chars] : tier_chars) {
        if (total_chars == 0 || chars * kPercentScale <= total_chars * kHeadingMaxDocCharPercent) {
            tiers.push_back(key);
        }
    }

    auto level_of = [&tiers, &tier_key](double size) -> int {
        const long key = tier_key(size);
        for (size_t i = 0; i < tiers.size(); ++i) {
            if (tiers[i] == key) {
                return static_cast<int>(std::min<size_t>(i + 1, kMaxHeadingLevel));
            }
        }
        return 0;
    };

    auto size_tier_cand = [body_size](const Block& b, size_t len) {
        return b.lines.size() <= kHeadingMaxLines && len > 0 && len <= kHeadingMaxChars &&
               b.size >= body_size * kHeadingMinSizeScale;
    };
    auto bold_line_cand = [body_size](const Block& b, size_t len) {
        return b.lines.size() == 1 && b.bold_majority && len > 0 && len <= kBoldHeadingMaxChars &&
               b.size >= body_size * kBoldHeadingMinSizeScale &&
               b.size < body_size * kHeadingMinSizeScale;
    };

    std::unordered_map<std::string, int> full_freq;
    std::unordered_map<std::string, int> label_freq;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || b.lines.empty()) {
            continue;
        }
        size_t len = 0;
        for (const Line& ln : b.lines) {
            len += ln.plain.size();
        }
        if (!size_tier_cand(b, len) && !bold_line_cand(b, len)) {
            continue;
        }
        ++full_freq[heading_norm_key(b)];
        std::string lab = heading_meta_label(b);
        if (!lab.empty()) {
            ++label_freq[std::move(lab)];
        }
    }

    double body_x0 = 0;
    double body_x1 = 0;
    {
        std::vector<double> xs0;
        std::vector<double> xs1;
        for (const Block& b : blocks) {
            if (b.kind == BlockKind::Paragraph && b.lines.size() >= 2) {
                xs0.push_back(b.x0);
                xs1.push_back(b.x1);
            }
        }
        if (!xs0.empty()) {
            std::sort(xs0.begin(), xs0.end());
            std::sort(xs1.begin(), xs1.end());
            body_x0 = xs0[xs0.size() / 10];
            body_x1 = xs1[xs1.size() * 9 / 10];
        }
    }
    const double body_w = body_x1 - body_x0;
    auto is_inset_callout = [body_x0, body_x1, body_w](const Block& b) {
        return body_w > 0 && (b.x0 - body_x0) >= kInsetCalloutMinRatio * body_w &&
               (body_x1 - b.x1) >= kInsetCalloutMinRatio * body_w;
    };

    const auto& const_full_freq = full_freq;
    const auto& const_label_freq = label_freq;
    auto titleish = [&const_full_freq, &const_label_freq](const Block& b) -> bool {
        if (b.lines.empty()) {
            return false;
        }
        const std::u32string& lastln = b.lines.back().plain;
        size_t e = lastln.size();
        while (e > 0 && lastln[e - 1] == U' ') {
            --e;
        }
        if (e > 0) {
            const char32_t last = lastln[e - 1];
            if (last == U'.' || last == U',' || last == U';' || last == U':' || last == U'!' ||
                last == U'?') {
                return false;
            }
        }
        if (is_citation_lead(b.lines.front()) || heading_is_caption(b)) {
            return false;
        }
        const std::string lab = heading_meta_label(b);
        if (!lab.empty()) {
            const auto lit = const_label_freq.find(lab);
            if (lit != const_label_freq.end() && lit->second >= kRepeatedHeadingMinCount) {
                return false;
            }
        }
        const auto fit = const_full_freq.find(heading_norm_key(b));
        if (fit != const_full_freq.end() && fit->second >= kRepeatedHeadingMinCount) {
            return false;
        }
        return true;
    };

    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph) {
            continue;
        }
        size_t len = 0;
        for (const Line& ln : b.lines) {
            len += ln.plain.size();
        }

        if (size_tier_cand(b, len)) {
            const int lvl = level_of(b.size);
            if (lvl > 0) {
                if (titleish(b)) {
                    b.kind = BlockKind::Heading;
                    b.heading_level = lvl;
                } else {
                    const char32_t last =
                        b.lines.back().plain.empty() ? U'\0' : b.lines.back().plain.back();
                    const bool sentence = last == U'.' || last == U'!' || last == U'?';
                    if (b.lines.size() >= 2 && sentence && is_inset_callout(b)) {
                        b.blockquote = true;
                    }
                }
                continue;
            }
        }
        if (bold_line_cand(b, len) && titleish(b)) {
            b.kind = BlockKind::Heading;
            b.heading_level = std::max(
                static_cast<int>(std::min<size_t>(tiers.size() + 1, kMaxHeadingLevel)),
                kMinBoldHeadingLevel);
        }
    }

    int anchor_depth = 0;
    int anchor_level = 0;
    bool have_anchor = false;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading) {
            continue;
        }
        const int d = heading_number_depth(b);
        if (d == 0) {
            continue;
        }
        if (!have_anchor || d < anchor_depth ||
            (d == anchor_depth && b.heading_level < anchor_level)) {
            anchor_depth = d;
            anchor_level = b.heading_level;
            have_anchor = true;
        }
    }
    if (have_anchor) {
        const int base = anchor_level - (anchor_depth - 1);
        for (Block& b : blocks) {
            if (b.kind != BlockKind::Heading) {
                continue;
            }
            const int d = heading_number_depth(b);
            if (d == 0) {
                continue;
            }
            b.heading_level = std::clamp(base + (d - 1), 1, kMaxHeadingLevel);
        }
    }

    std::unordered_map<std::u32string, int> heading_number_level;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading || b.lines.empty()) {
            continue;
        }
        SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth == 0) {
            continue;
        }
        const auto it = heading_number_level.find(sn.canon);
        if (it == heading_number_level.end() || b.heading_level < it->second) {
            heading_number_level.insert_or_assign(std::move(sn.canon), b.heading_level);
        }
    }
    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || !b.bold_majority || b.lines.size() != 1) {
            continue;
        }
        const size_t len = b.lines.front().plain.size();
        if (len == 0 || len > kRunInHeadingMaxChars) {
            continue;
        }
        const SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth < 2) {
            continue;
        }
        const size_t dot = sn.canon.find_last_of(U'.');
        if (dot == std::u32string::npos) {
            continue;
        }
        const auto it = heading_number_level.find(sn.canon.substr(0, dot));
        if (it == heading_number_level.end() || !titleish(b)) {
            continue;
        }
        b.kind = BlockKind::Heading;
        b.heading_level = std::clamp(it->second + 1, 1, kMaxHeadingLevel);
    }
}

void relocate_footnotes(std::vector<Block>& blocks) {
    int last_page = 0;
    for (const Block& b : blocks) {
        last_page = std::max(last_page, b.page);
    }
    std::vector<Block> kept;
    std::vector<Block> notes;
    kept.reserve(blocks.size());
    for (Block& b : blocks) {
        if (b.is_footnote && b.footnote_numbered) {
            b.page = last_page;
            notes.push_back(std::move(b));
        } else {
            kept.push_back(std::move(b));
        }
    }
    for (Block& n : notes) {
        kept.push_back(std::move(n));
    }
    blocks = std::move(kept);
}

bool is_lone_math_atom(const Block& b) noexcept {
    if (b.kind != BlockKind::Paragraph || b.is_footnote || b.blockquote || b.lines.size() != 1) {
        return false;
    }
    bool has_math = false;
    for (const StyledRun& r : b.lines.front().runs) {
        if (r.math) {
            has_math = true;
            continue;
        }
        for (char32_t c : r.text) {
            if (c == U' ') {
                continue;
            }
            if (is_letter_cp(c) || (c >= U'0' && c <= U'9')) {
                return false;
            }
        }
    }
    return has_math;
}

void merge_trailing_math_atoms(std::vector<Block>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.kind != BlockKind::Paragraph || prev.is_footnote) {
            continue;
        }
        if (prev.lines.empty() || cur.lines.empty() || !is_lone_math_atom(cur)) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) >
            kLoneMathSizeDiffRatio * std::max(prev.size, cur.size)) {
            continue;
        }
        const std::u32string& ptext = prev.lines.back().plain;
        size_t e = ptext.size();
        while (e > 0 && ptext[e - 1] == U' ') {
            --e;
        }
        if (e == 0) {
            continue;
        }
        const char32_t last = ptext[e - 1];
        const bool continues = is_letter_cp(last) || (last >= U'0' && last <= U'9') ||
                               last == U',' || last == U'-' || last == kUnicodeHyphenCp ||
                               last == kSoftHyphenCp;
        if (!continues) {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

bool is_table_continuation_marker(const Block& b) noexcept {
    if (b.kind != BlockKind::Paragraph || b.lines.empty()) {
        return false;
    }
    bool any = false;
    for (const Line& ln : b.lines) {
        for (char32_t c : ln.plain) {
            if (c == U' ') {
                continue;
            }
            if (c != kContBelowTriangleCp && c != kContAboveTriangleCp) {
                return false;
            }
            any = true;
        }
    }
    return any;
}

bool same_column_signature(const Block& a, const Block& b) noexcept {
    if (a.col_edges.empty() || a.col_edges.size() != b.col_edges.size()) {
        return false;
    }
    const double tol = std::max(kTableSigMinTolPt, kTableSigTolScale * std::max(a.size, kMinFontSizePt));
    for (size_t i = 0; i < a.col_edges.size(); ++i) {
        if (std::fabs(a.col_edges[i] - b.col_edges[i]) > tol) {
            return false;
        }
    }
    return true;
}

bool table_row_is_header(std::span<const TableCell> row) noexcept {
    size_t nonempty = 0;
    size_t bold = 0;
    for (const TableCell& cell : row) {
        bool has_ink = false;
        bool all_bold = true;
        for (const StyledRun& r : cell) {
            for (char32_t c : r.text) {
                if (c != U' ') {
                    has_ink = true;
                    if (!r.bold) {
                        all_bold = false;
                    }
                }
            }
        }
        if (!has_ink) {
            continue;
        }
        ++nonempty;
        if (all_bold) {
            ++bold;
        }
    }
    return nonempty > 0 && bold * 2 >= nonempty;
}

void merge_ruled_table_continuations(std::vector<Block>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].kind != BlockKind::Table || blocks[i].col_edges.empty()) {
            continue;
        }
        const bool prev_header = !blocks[i].table_cells.empty() &&
                                 table_row_is_header(blocks[i].table_cells.front());
        int last_page = blocks[i].page;
        while (true) {
            size_t j = i + 1;
            bool marker_between = false;
            while (j < blocks.size() && is_table_continuation_marker(blocks[j])) {
                marker_between = true;
                ++j;
            }
            if (j >= blocks.size()) {
                break;
            }
            Block& later = blocks[j];
            if (later.kind != BlockKind::Table || later.col_edges.empty()) {
                break;
            }
            if (later.page != last_page + 1) {
                break;
            }
            if (!same_column_signature(blocks[i], later)) {
                break;
            }
            const bool later_header =
                !later.table_cells.empty() && table_row_is_header(later.table_cells.front());
            if (!(marker_between || (prev_header && !later_header))) {
                break;
            }

            for (auto& row : later.table_cells) {
                blocks[i].table_cells.push_back(std::move(row));
            }
            blocks[i].y_bottom = later.y_bottom;
            last_page = later.page;
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                         blocks.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        }
    }
}

std::optional<size_t> trailing_page_number(const Line& ln) noexcept {
    std::ptrdiff_t hi = -1;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(ln.chars.size()) - 1; i >= 0; --i) {
        if (!ln.chars[static_cast<size_t>(i)].space) {
            hi = i;
            break;
        }
    }
    if (hi < 0 || !is_ascii_digit(ln.chars[static_cast<size_t>(hi)].code)) {
        return std::nullopt;
    }
    size_t lo = static_cast<size_t>(hi);
    while (lo > 0 && !ln.chars[lo - 1].space && is_ascii_digit(ln.chars[lo - 1].code)) {
        --lo;
    }
    std::ptrdiff_t p = static_cast<std::ptrdiff_t>(lo) - 1;
    while (p >= 0 && ln.chars[static_cast<size_t>(p)].space) {
        --p;
    }
    if (p < 0) {
        return std::nullopt;
    }
    const double wgap = ln.word_gap > 0 ? ln.word_gap : kDefaultWordGapScale * ln.size;
    bool space_between = false;
    for (size_t k = static_cast<size_t>(p) + 1; k < lo; ++k) {
        if (ln.chars[k].space) {
            space_between = true;
            break;
        }
    }
    if (!space_between &&
        ln.chars[lo].left - ln.chars[static_cast<size_t>(p)].right < wgap) {
        return std::nullopt;
    }
    return lo;
}

void fold_toc_orphan_rows(std::vector<Block>& blocks) {
    auto two_col_toc_table = [](const Block& b) {
        if (b.kind != BlockKind::Table || !b.col_edges.empty() || b.table_cells.empty()) {
            return false;
        }
        size_t maxcol = 0;
        for (const auto& row : b.table_cells) {
            maxcol = std::max(maxcol, row.size());
        }
        return maxcol == 2;
    };
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& P = blocks[i];
        if (P.kind != BlockKind::Paragraph || P.lines.size() != 1) {
            continue;
        }
        const Line& ln = P.lines[0];
        if (ln.segments.size() >= 2) {
            continue;
        }
        Block& T1 = blocks[i - 1];
        if (!two_col_toc_table(T1) || T1.page != P.page) {
            continue;
        }
        const std::optional<size_t> ns = trailing_page_number(ln);
        if (!ns || *ns == 0) {
            continue;
        }
        const double ref_size = std::max(T1.size, kMinFontSizePt);
        const double tol = std::max(kTocAlignMinTolPt, kTocAlignTolScale * ref_size);
        const double vgap = kTocMaxVGapScale * ref_size;
        if (std::fabs(P.x1 - T1.x1) > tol || P.x0 < T1.x0 - tol || T1.y_bottom - P.y_top > vgap) {
            continue;
        }
        std::vector<TableCell> row(2);
        row[0] = render_cell_runs(ln, 0, *ns - 1);
        row[1] = render_cell_runs(ln, *ns, ln.chars.size() - 1);
        if (row[0].empty() || row[1].empty()) {
            continue;
        }
        T1.table_cells.push_back(std::move(row));
        T1.x1 = std::max(T1.x1, P.x1);
        T1.y_bottom = std::min(T1.y_bottom, P.y_bottom);
        std::ptrdiff_t erase_end = static_cast<std::ptrdiff_t>(i) + 1;
        if (i + 1 < blocks.size()) {
            Block& T2 = blocks[i + 1];
            if (two_col_toc_table(T2) && T2.page == P.page && std::fabs(T2.x1 - T1.x1) <= tol &&
                T2.x0 >= T1.x0 - tol && P.y_bottom - T2.y_top <= vgap) {
                for (auto& r : T2.table_cells) {
                    T1.table_cells.push_back(std::move(r));
                }
                T1.x0 = std::min(T1.x0, T2.x0);
                T1.x1 = std::max(T1.x1, T2.x1);
                T1.y_bottom = std::min(T1.y_bottom, T2.y_bottom);
                erase_end = static_cast<std::ptrdiff_t>(i) + 2;
            }
        }
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i),
                     blocks.begin() + erase_end);
        --i;
    }
}

void merge_reference_entries(std::vector<Block>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.kind != BlockKind::Paragraph || prev.is_footnote || prev.blockquote) {
            continue;
        }
        if (cur.kind != BlockKind::Paragraph || cur.is_footnote || cur.blockquote) {
            continue;
        }
        if (prev.lines.empty() || cur.lines.empty()) {
            continue;
        }
        if (!is_citation_lead(prev.lines.front()) || is_citation_lead(cur.lines.front())) {
            continue;
        }
        if (cur.page != prev.page && cur.page != prev.page + 1) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) > kRefEntrySizeDiffRatio * std::max(prev.size, cur.size)) {
            continue;
        }
        const std::u32string& ptext = prev.lines.back().plain;
        size_t e = ptext.size();
        while (e > 0 && ptext[e - 1] == U' ') {
            --e;
        }
        if (e == 0) {
            continue;
        }
        const char32_t last = ptext[e - 1];
        if (last == U'.' || last == U'!' || last == U'?') {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

void merge_cross_page_paragraphs(std::vector<Block>& blocks) {
    constexpr std::u32string_view kSentenceEnders = U".!?:;\u201D\u2019\"')]";
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.page + 1 != cur.page) {
            continue;
        }
        if (prev.kind != BlockKind::Paragraph || cur.kind != BlockKind::Paragraph) {
            continue;
        }
        if (prev.is_footnote || cur.is_footnote || prev.lines.empty() || cur.lines.empty()) {
            continue;
        }
        if (std::fabs(prev.size - cur.size) >
            kCrossPageSizeDiffRatio * std::max(prev.size, cur.size)) {
            continue;
        }

        const std::u32string& ptext = prev.lines.back().plain;
        const std::u32string& ctext = cur.lines.front().plain;
        if (ptext.empty() || ctext.empty()) {
            continue;
        }
        const char32_t last = ptext.back();
        const char32_t first = ctext.front();
        const bool scoped = last == U':' && ptext.size() >= 2 && ptext[ptext.size() - 2] == U':';
        const bool continues =
            scoped ? (is_ascii_alpha(first) || is_ascii_digit(first) || first == U'_')
                   : (kSentenceEnders.find(last) == std::u32string_view::npos) &&
                         (is_ascii_lower(first) || last == U'-' || last == kSoftHyphenCp);
        if (!continues) {
            continue;
        }

        for (Line& ln : cur.lines) {
            prev.lines.push_back(std::move(ln));
        }
        finalize_block_geometry(prev);
        prev.page = cur.page;
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

void insert_image_blocks(std::vector<Block>& blocks, const std::vector<PageData>& pages) {
    for (const PageData& page : pages) {
        for (const ImageRef& img : page.images) {
            Block b;
            b.kind = BlockKind::Image;
            b.page = page.index;
            b.image_path = img.path;
            b.y_top = img.y_top;
            b.y_bottom = img.y_bottom;
            b.x0 = img.x;

            const double anchor = (img.y_top + img.y_bottom) / 2.0;
            auto pos = blocks.end();
            auto best = blocks.end();
            double best_top = -kCoordSentinel;
            for (auto it = blocks.begin(); it != blocks.end(); ++it) {
                if (it->page != page.index) {
                    if (it->page > page.index && pos == blocks.end()) {
                        pos = it;
                    }
                    continue;
                }
                if (it->y_top < anchor && it->y_top > best_top) {
                    best_top = it->y_top;
                    best = it;
                }
            }
            if (best != blocks.end()) {
                pos = best;
            }
            blocks.insert(pos, std::move(b));
        }
    }
}

class MergeRuledTableContinuationsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        merge_ruled_table_continuations(doc.blocks);
    }
};

class FoldTocOrphanRowsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        fold_toc_orphan_rows(doc.blocks);
    }
};

class StripHeadersFootersPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.strip_headers_footers;
    }
    void apply(Document& doc, const PassContext& ctx) const override {
        strip_headers_footers(doc.blocks, ctx.pages);
    }
};

class ClassifyHeadingsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const noexcept override {
        return params.detect_headings;
    }
    void apply(Document& doc, const PassContext&) const override {
        classify_headings(doc.blocks, doc.body_size);
    }
};

class RelocateFootnotesPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        relocate_footnotes(doc.blocks);
    }
};

class MergeTrailingMathAtomsPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_trailing_math_atoms(doc.blocks);
    }
};

class MergeReferenceEntriesPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_reference_entries(doc.blocks);
    }
};

class MergeCrossPageParagraphsPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext&) const override {
        merge_cross_page_paragraphs(doc.blocks);
    }
};

class InsertImageBlocksPass final : public DocumentPass {
protected:
    void apply(Document& doc, const PassContext& ctx) const override {
        insert_image_blocks(doc.blocks, ctx.pages);
    }
};

}  // namespace

DocumentPass::~DocumentPass() = default;

double compute_body_size(const std::vector<PageData>& pages) {
    // Flat stack histogram for common font sizes [0.00pt .. 127.75pt] in
    // quarter-point steps, with a small overflow vector for giant display type.
    std::array<size_t, kFastHistBuckets> fast_hist{};
    std::vector<long> overflow_keys;
    bool any = false;

    for (const PageData& page : pages) {
        for (const CharInfo& c : page.chars) {
            if (c.space || c.size <= 0) {
                continue;
            }
            any = true;
            const long q = std::lround(c.size * kQuarterPtScale);
            if (q >= 0 && static_cast<size_t>(q) < kFastHistBuckets) {
                ++fast_hist[static_cast<size_t>(q)];
            } else {
                overflow_keys.push_back(q);
            }
        }
    }
    if (!any) {
        return kDefaultBodySizePt;
    }

    long best_q = 0;
    size_t best_count = 0;
    for (size_t q = 0; q < kFastHistBuckets; ++q) {
        if (fast_hist[q] > best_count) {
            best_count = fast_hist[q];
            best_q = static_cast<long>(q);
        }
    }
    if (!overflow_keys.empty()) {
        std::sort(overflow_keys.begin(), overflow_keys.end());
        size_t i = 0;
        while (i < overflow_keys.size()) {
            size_t j = i + 1;
            while (j < overflow_keys.size() && overflow_keys[j] == overflow_keys[i]) {
                ++j;
            }
            const size_t cnt = j - i;
            if (cnt > best_count || (cnt == best_count && overflow_keys[i] < best_q)) {
                best_count = cnt;
                best_q = overflow_keys[i];
            }
            i = j;
        }
    }
    return static_cast<double>(best_q) / kQuarterPtScale;
}

void strip_running_chrome(std::vector<PageLines>& pages) {
    if (pages.size() < kMinPagesForLineChrome) {
        return;
    }

    std::map<std::pair<int, std::string>, std::set<int>> seen;
    std::map<std::tuple<int, long, long, std::string>, std::set<int>> pos_seen;
    std::map<std::tuple<long, long, std::string>, std::set<int>> wm_seen;
    std::pair<size_t, size_t> seg_fallback{0, 0};

    for (const PageLines& pg : pages) {
        for (const auto& region : pg.regions) {
            for (const Line& ln : region) {
                const int band = chrome_band_of(ln, pg.height);
                for (const auto& [f, l] : chrome_segments_of(ln, seg_fallback)) {
                    if (band != 0) {
                        std::string sig = chrome_segment_sig(ln, f, l);
                        if (sig.empty()) {
                            continue;
                        }
                        const auto [qx, qy] = chrome_position_key(ln, f);
                        seen[{band, sig}].insert(pg.index);
                        pos_seen[{band, qx, qy, std::move(sig)}].insert(pg.index);
                    } else {
                        if (!chrome_in_margin(ln, f, l, pg.width)) {
                            continue;
                        }
                        std::string sig = chrome_watermark_sig(ln, f, l);
                        if (sig.empty()) {
                            continue;
                        }
                        const auto [qx, qy] = chrome_position_key(ln, f);
                        wm_seen[{qx, qy, std::move(sig)}].insert(pg.index);
                    }
                }
            }
        }
    }

    const size_t threshold = std::max<size_t>(kMinChromeRepeatCount, (pages.size() + 1) / 2);
    std::set<std::pair<int, std::string>> chrome;
    for (const auto& [key, pset] : seen) {
        if (pset.size() >= threshold) {
            chrome.insert(key);
        }
    }

    std::set<std::tuple<int, long, long, std::string>> pos_chrome;
    for (const auto& [key, pset] : pos_seen) {
        if (pset.size() >= kPosBandThreshold) {
            pos_chrome.insert(key);
        }
    }

    const size_t wm_threshold = std::max<size_t>(kMinChromeRepeatCount, (pages.size() + 3) / 4);
    std::set<std::string> watermark_sigs;
    for (const auto& [key, pset] : wm_seen) {
        const std::string& sig = std::get<2>(key);
        if (sig.size() < kWatermarkMinSigLen || sig.size() > kWatermarkMaxSigLen) {
            continue;
        }
        if (pset.size() >= wm_threshold) {
            watermark_sigs.insert(sig);
        }
    }
    if (!watermark_sigs.empty() && has_env_flag("PDF2MD_DEBUG_WM")) {
        for (const auto& s : watermark_sigs) {
            std::fprintf(stderr, "WM sig %s\n", s.c_str());
        }
    }

    // Watermark removal per column segment: evaluate drop spans against the
    // unmutated page first, then compact stripped glyphs.
    if (!watermark_sigs.empty()) {
        for (PageLines& pg : pages) {
            const PageLines& const_pg = pg;
            std::vector<std::vector<std::vector<std::pair<size_t, size_t>>>> page_drops(
                pg.regions.size());
            bool page_any = false;
            for (size_t ri = 0; ri < const_pg.regions.size(); ++ri) {
                const auto& region = const_pg.regions[ri];
                page_drops[ri].resize(region.size());
                for (size_t li = 0; li < region.size(); ++li) {
                    const Line& ln = region[li];
                    if (chrome_band_of(ln, const_pg.height) != 0) {
                        continue;
                    }
                    for (const auto& [f, l] : chrome_segments_of(ln, seg_fallback)) {
                        if (!chrome_in_margin(ln, f, l, const_pg.width)) {
                            continue;
                        }
                        const std::string sig = chrome_watermark_sig(ln, f, l);
                        if (sig.empty() || !watermark_sigs.contains(sig)) {
                            continue;
                        }
                        const size_t safe_l = std::min(l, ln.chars.size() - 1);
                        if (!chrome_isolated_span(ln.chars[f].left, ln.chars[safe_l].right,
                                                  ln.baseline, ln, const_pg)) {
                            continue;
                        }
                        page_drops[ri][li].emplace_back(f, safe_l);
                        page_any = true;
                    }
                }
            }
            if (!page_any) {
                continue;
            }
            for (size_t ri = 0; ri < pg.regions.size(); ++ri) {
                auto& region = pg.regions[ri];
                for (size_t li = 0; li < region.size(); ++li) {
                    const auto& drop = page_drops[ri][li];
                    if (drop.empty()) {
                        continue;
                    }
                    Line& ln = region[li];
                    std::vector<CharInfo> kept;
                    kept.reserve(ln.chars.size());
                    for (size_t i = 0; i < ln.chars.size(); ++i) {
                        bool in_drop = false;
                        for (const auto& [f, l] : drop) {
                            if (i >= f && i <= l) {
                                in_drop = true;
                                break;
                            }
                        }
                        if (!in_drop) {
                            kept.push_back(ln.chars[i]);
                        }
                    }
                    ln.chars = std::move(kept);
                    const bool has_text = std::any_of(ln.chars.cbegin(), ln.chars.cend(),
                                                      [](const CharInfo& c) { return !c.space; });
                    if (has_text) {
                        finalize_line(ln);
                        build_runs(ln);
                    } else {
                        ln.chars.clear();
                        ln.plain.clear();
                        ln.runs.clear();
                        ln.segments.clear();
                    }
                }
                std::erase_if(region, [](const Line& ln) { return ln.plain.empty(); });
            }
        }
    }

    if (chrome.empty() && pos_chrome.empty()) {
        return;
    }

    // Evaluate band-line drops across the unmutated page first so
    // chrome_isolated_span never inspects half-erased vectors during remove_if.
    for (PageLines& pg : pages) {
        const PageLines& const_pg = pg;
        std::vector<std::vector<char>> drop_line(const_pg.regions.size());
        for (size_t ri = 0; ri < const_pg.regions.size(); ++ri) {
            const auto& region = const_pg.regions[ri];
            drop_line[ri].assign(region.size(), 0);
            for (size_t li = 0; li < region.size(); ++li) {
                const Line& ln = region[li];
                const int band = chrome_band_of(ln, const_pg.height);
                if (band == 0) {
                    continue;
                }
                bool any = false;
                bool all_chrome = true;
                for (const auto& [f, l] : chrome_segments_of(ln, seg_fallback)) {
                    const std::string sig = chrome_segment_sig(ln, f, l);
                    if (sig.empty()) {
                        continue;
                    }
                    if (!chrome.contains({band, sig})) {
                        const auto [qx, qy] = chrome_position_key(ln, f);
                        if (!pos_chrome.contains({band, qx, qy, sig})) {
                            all_chrome = false;
                            break;
                        }
                        const double dist = std::max(kPosChromeIsolationLineScale * ln.size,
                                                     kPosChromeMinIsolationDistPt);
                        const size_t safe_l = std::min(l, ln.chars.size() - 1);
                        if (!chrome_isolated_span(ln.chars[f].left, ln.chars[safe_l].right,
                                                  ln.baseline, ln, const_pg, dist)) {
                            all_chrome = false;
                            break;
                        }
                    }
                    any = true;
                }
                if (any && all_chrome) {
                    drop_line[ri][li] = 1;
                }
            }
        }
        for (size_t ri = 0; ri < pg.regions.size(); ++ri) {
            auto& region = pg.regions[ri];
            const auto& flags = drop_line[ri];
            size_t out_idx = 0;
            for (size_t li = 0; li < region.size(); ++li) {
                if (!flags[li]) {
                    if (out_idx != li) {
                        region[out_idx] = std::move(region[li]);
                    }
                    ++out_idx;
                }
            }
            region.resize(out_idx);
        }
    }
}

// Factory Method: assembles the document-cleanup pipeline. THE ORDER IS A
// CONTRACT. Several passes depend on running before or after another, and the
// output stays byte-identical only when this exact sequence is preserved.
std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline() {
    std::vector<std::unique_ptr<DocumentPass>> passes;
    passes.reserve(kPipelinePassCount);
    passes.push_back(std::make_unique<MergeRuledTableContinuationsPass>());
    passes.push_back(std::make_unique<FoldTocOrphanRowsPass>());
    passes.push_back(std::make_unique<StripHeadersFootersPass>());
    passes.push_back(std::make_unique<ClassifyHeadingsPass>());
    passes.push_back(std::make_unique<RelocateFootnotesPass>());
    passes.push_back(std::make_unique<MergeTrailingMathAtomsPass>());
    passes.push_back(std::make_unique<MergeReferenceEntriesPass>());
    passes.push_back(std::make_unique<MergeCrossPageParagraphsPass>());
    passes.push_back(std::make_unique<InsertImageBlocksPass>());
    return passes;
}

}  // namespace detail
}  // namespace pdf2md
