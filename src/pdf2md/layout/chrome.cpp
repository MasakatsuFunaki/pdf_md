#include "pdf2md/layout/chrome.h"

#include "pdf2md/layout/line_cluster.h"
#include "pdf2md/layout/math.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf2md::layout {

using model::has_env_flag;
using model::kCoordSentinel;
using model::kDefaultBodySizePt;

namespace {

constexpr char32_t kAsciiLimitCp = 0x80U;

// Body-size histogram quantization (quarter-point buckets).
constexpr double kQuarterPtScale = 4.0;
constexpr std::size_t kFastHistBuckets = 512U;

// Running-chrome and watermark detection constants.
constexpr double kHeaderBandBottomRatio = 0.88;
constexpr double kFooterBandTopRatio = 0.12;
constexpr double kChromeQuantizeBinPt = 4.0;
constexpr double kDefaultIsolationDistPt = 24.0;
constexpr double kMarginLeftRatio = 0.15;
constexpr double kMarginRightRatio = 0.85;
constexpr std::size_t kMinPagesForLineChrome = 3U;
constexpr std::size_t kMinPagesForBlockChrome = 2U;
constexpr std::size_t kMinChromeRepeatCount = 3U;
constexpr std::size_t kPosBandThreshold = 4U;
constexpr std::size_t kWatermarkMinSigLen = 2U;
constexpr std::size_t kWatermarkMaxSigLen = 24U;
constexpr double kPosChromeIsolationLineScale = 1.8;
constexpr double kPosChromeMinIsolationDistPt = 14.0;

std::string normalized_block_text(const Block& b) {
    std::string out;
    for (const Line& ln : b.lines) {
        for (const char32_t cp : ln.plain) {
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
    for (const std::string_view p : kPatterns) {
        if (norm == p) {
            return true;
        }
    }
    return false;
}

std::string chrome_segment_sig(const Line& ln, std::size_t first, std::size_t last) {
    std::string out;
    for (std::size_t i = first; i <= last && i < ln.chars.size(); ++i) {
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
    if (h <= 0.0 || ln.chars.empty()) {
        return 0;
    }
    double top = -kCoordSentinel;
    double bottom = kCoordSentinel;
    for (const CharInfo& c : ln.chars) {
        top = (std::max)(top, c.top);
        bottom = (std::min)(bottom, c.bottom);
    }
    if (bottom > kHeaderBandBottomRatio * h) {
        return 1;
    }
    if (top < kFooterBandTopRatio * h) {
        return 2;
    }
    return 0;
}

std::span<const std::pair<std::size_t, std::size_t>> chrome_segments_of(
    const Line& ln, std::pair<std::size_t, std::size_t>& fallback) noexcept {
    if (!ln.segments.empty()) {
        return ln.segments;
    }
    if (!ln.chars.empty()) {
        fallback = {0U, ln.chars.size() - 1U};
        return std::span<const std::pair<std::size_t, std::size_t>>(&fallback, 1U);
    }
    return {};
}

std::pair<long, long> chrome_position_key(const Line& ln, std::size_t first) noexcept {
    if (first >= ln.chars.size()) {
        return {0L, 0L};
    }
    return {std::lround(ln.chars[first].left / kChromeQuantizeBinPt),
            std::lround(ln.baseline / kChromeQuantizeBinPt)};
}

std::string chrome_watermark_sig(const Line& ln, std::size_t first, std::size_t last) {
    std::string out;
    for (std::size_t i = first; i <= last && i < ln.chars.size(); ++i) {
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

bool chrome_in_margin(const Line& ln, std::size_t first, std::size_t last,
                      double page_w) noexcept {
    if (page_w <= 0.0 || ln.chars.empty() || first >= ln.chars.size()) {
        return false;
    }
    const double x0 = ln.chars[first].left;
    const double x1 = ln.chars[(std::min)(last, ln.chars.size() - 1U)].right;
    return x1 < kMarginLeftRatio * page_w || x0 > kMarginRightRatio * page_w;
}

struct PageCounter {
    int last_page = -1;
    std::size_t page_count = 0U;

    void observe(int page_index) noexcept {
        if (last_page != page_index) {
            last_page = page_index;
            ++page_count;
        }
    }
};

constexpr std::size_t hash_mix(std::size_t seed, std::size_t value) noexcept {
    constexpr std::size_t kGoldenRatioMix = static_cast<std::size_t>(0x9E3779B97F4A7C15ULL);
    return seed ^ (value + kGoldenRatioMix + (seed << 6U) + (seed >> 2U));
}

struct BandSigKey {
    int band = 0;
    std::string sig;

    bool operator==(const BandSigKey& other) const noexcept {
        return band == other.band && sig == other.sig;
    }
};

struct BandSigHash {
    std::size_t operator()(const BandSigKey& k) const noexcept {
        return hash_mix(std::hash<int>{}(k.band), std::hash<std::string>{}(k.sig));
    }
};

struct PosSigKey {
    int band = 0;
    long qx = 0L;
    long qy = 0L;
    std::string sig;

    bool operator==(const PosSigKey& other) const noexcept {
        return band == other.band && qx == other.qx && qy == other.qy && sig == other.sig;
    }
};

struct PosSigHash {
    std::size_t operator()(const PosSigKey& k) const noexcept {
        std::size_t h = std::hash<int>{}(k.band);
        h = hash_mix(h, std::hash<long>{}(k.qx));
        h = hash_mix(h, std::hash<long>{}(k.qy));
        return hash_mix(h, std::hash<std::string>{}(k.sig));
    }
};

struct WmSigKey {
    long qx = 0L;
    long qy = 0L;
    std::string sig;

    bool operator==(const WmSigKey& other) const noexcept {
        return qx == other.qx && qy == other.qy && sig == other.sig;
    }
};

struct WmSigHash {
    std::size_t operator()(const WmSigKey& k) const noexcept {
        std::size_t h = std::hash<long>{}(k.qx);
        h = hash_mix(h, std::hash<long>{}(k.qy));
        return hash_mix(h, std::hash<std::string>{}(k.sig));
    }
};

}  // namespace

double compute_body_size(const std::vector<PageData>& pages) {
    std::array<std::size_t, kFastHistBuckets> fast_hist{};
    std::vector<long> overflow_keys;
    bool any = false;

    for (const PageData& page : pages) {
        for (const CharInfo& c : page.chars) {
            if (c.space || c.size <= 0.0) {
                continue;
            }
            any = true;
            const long q = std::lround(c.size * kQuarterPtScale);
            if (q >= 0L && static_cast<std::size_t>(q) < kFastHistBuckets) {
                ++fast_hist[static_cast<std::size_t>(q)];
            } else {
                overflow_keys.push_back(q);
            }
        }
    }
    if (!any) {
        return kDefaultBodySizePt;
    }

    long best_q = 0L;
    std::size_t best_count = 0U;
    for (std::size_t q = 0U; q < kFastHistBuckets; ++q) {
        if (fast_hist[q] > best_count) {
            best_count = fast_hist[q];
            best_q = static_cast<long>(q);
        }
    }
    if (!overflow_keys.empty()) {
        std::sort(overflow_keys.begin(), overflow_keys.end());
        std::size_t i = 0U;
        while (i < overflow_keys.size()) {
            std::size_t j = i + 1U;
            while (j < overflow_keys.size() && overflow_keys[j] == overflow_keys[i]) {
                ++j;
            }
            const std::size_t cnt = j - i;
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

    std::unordered_map<BandSigKey, PageCounter, BandSigHash> seen;
    std::unordered_map<PosSigKey, PageCounter, PosSigHash> pos_seen;
    std::unordered_map<WmSigKey, PageCounter, WmSigHash> wm_seen;
    seen.reserve(pages.size() * 4U);
    pos_seen.reserve(pages.size() * 4U);
    std::pair<std::size_t, std::size_t> seg_fallback{0U, 0U};

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
                        seen[BandSigKey{band, sig}].observe(pg.index);
                        pos_seen[PosSigKey{band, qx, qy, std::move(sig)}].observe(pg.index);
                    } else {
                        if (!chrome_in_margin(ln, f, l, pg.width)) {
                            continue;
                        }
                        std::string sig = chrome_watermark_sig(ln, f, l);
                        if (sig.empty()) {
                            continue;
                        }
                        const auto [qx, qy] = chrome_position_key(ln, f);
                        wm_seen[WmSigKey{qx, qy, std::move(sig)}].observe(pg.index);
                    }
                }
            }
        }
    }

    const std::size_t threshold = (std::max)(kMinChromeRepeatCount, (pages.size() + 1U) / 2U);
    const bool has_chrome = std::any_of(seen.cbegin(), seen.cend(), [threshold](const auto& kv) {
        return kv.second.page_count >= threshold;
    });
    const bool has_pos_chrome = std::any_of(pos_seen.cbegin(), pos_seen.cend(), [](const auto& kv) {
        return kv.second.page_count >= kPosBandThreshold;
    });

    const std::size_t wm_threshold = (std::max)(kMinChromeRepeatCount, (pages.size() + 3U) / 4U);
    std::unordered_set<std::string> watermark_sigs;
    for (const auto& [key, ctr] : wm_seen) {
        if (key.sig.size() < kWatermarkMinSigLen || key.sig.size() > kWatermarkMaxSigLen) {
            continue;
        }
        if (ctr.page_count >= wm_threshold) {
            watermark_sigs.insert(key.sig);
        }
    }
    if (!watermark_sigs.empty() && has_env_flag("PDF2MD_DEBUG_WM")) {
        std::vector<std::string_view> sorted_sigs(watermark_sigs.cbegin(), watermark_sigs.cend());
        std::sort(sorted_sigs.begin(), sorted_sigs.end());
        for (const std::string_view s : sorted_sigs) {
            std::fprintf(stderr, "WM sig %.*s\n", static_cast<int>(s.size()), s.data());
        }
    }

    if (!watermark_sigs.empty()) {
        for (PageLines& pg : pages) {
            const PageLines& const_pg = pg;
            std::vector<std::vector<std::vector<std::pair<std::size_t, std::size_t>>>> page_drops(
                pg.regions.size());
            bool page_any = false;
            for (std::size_t ri = 0U; ri < const_pg.regions.size(); ++ri) {
                const auto& region = const_pg.regions[ri];
                page_drops[ri].resize(region.size());
                for (std::size_t li = 0U; li < region.size(); ++li) {
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
                        const std::size_t safe_l = (std::min)(l, ln.chars.size() - 1U);
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
            for (std::size_t ri = 0U; ri < pg.regions.size(); ++ri) {
                auto& region = pg.regions[ri];
                for (std::size_t li = 0U; li < region.size(); ++li) {
                    const auto& drop = page_drops[ri][li];
                    if (drop.empty()) {
                        continue;
                    }
                    Line& ln = region[li];
                    std::vector<CharInfo> kept;
                    kept.reserve(ln.chars.size());
                    for (std::size_t i = 0U; i < ln.chars.size(); ++i) {
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
                    const bool has_text =
                        std::any_of(ln.chars.cbegin(), ln.chars.cend(),
                                    [](const CharInfo& c) noexcept { return !c.space; });
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

    if (!has_chrome && !has_pos_chrome) {
        return;
    }

    const auto& const_seen = seen;
    const auto& const_pos_seen = pos_seen;
    for (PageLines& pg : pages) {
        const PageLines& const_pg = pg;
        std::vector<std::vector<char>> drop_line(const_pg.regions.size());
        for (std::size_t ri = 0U; ri < const_pg.regions.size(); ++ri) {
            const auto& region = const_pg.regions[ri];
            drop_line[ri].assign(region.size(), 0);
            for (std::size_t li = 0U; li < region.size(); ++li) {
                const Line& ln = region[li];
                const int band = chrome_band_of(ln, const_pg.height);
                if (band == 0) {
                    continue;
                }
                bool any = false;
                bool all_chrome = true;
                for (const auto& [f, l] : chrome_segments_of(ln, seg_fallback)) {
                    std::string sig = chrome_segment_sig(ln, f, l);
                    if (sig.empty()) {
                        continue;
                    }
                    const auto sit = const_seen.find(BandSigKey{band, sig});
                    if (sit == const_seen.end() || sit->second.page_count < threshold) {
                        const auto [qx, qy] = chrome_position_key(ln, f);
                        const auto pit =
                            const_pos_seen.find(PosSigKey{band, qx, qy, std::move(sig)});
                        if (pit == const_pos_seen.end() ||
                            pit->second.page_count < kPosBandThreshold) {
                            all_chrome = false;
                            break;
                        }
                        const double dist = (std::max)(kPosChromeIsolationLineScale * ln.size,
                                                       kPosChromeMinIsolationDistPt);
                        const std::size_t safe_l = (std::min)(l, ln.chars.size() - 1U);
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
        for (std::size_t ri = 0U; ri < pg.regions.size(); ++ri) {
            auto& region = pg.regions[ri];
            const auto& flags = drop_line[ri];
            std::size_t out_idx = 0U;
            for (std::size_t li = 0U; li < region.size(); ++li) {
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

void strip_headers_footers(std::vector<Block>& blocks, const std::vector<PageData>& pages) {
    if (pages.size() < kMinPagesForBlockChrome) {
        return;
    }
    std::unordered_map<int, std::pair<double, double>> page_dims;
    page_dims.reserve(pages.size());
    for (const PageData& p : pages) {
        page_dims.insert_or_assign(p.index, std::make_pair(p.width, p.height));
    }

    const auto zone_of = [&page_dims](const Block& b) -> int {
        const auto it = page_dims.find(b.page);
        if (it == page_dims.end()) {
            return 0;
        }
        const double h = it->second.second;
        if (h <= 0.0) {
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

    std::unordered_map<BandSigKey, PageCounter, BandSigHash> seen;
    seen.reserve(pages.size() * 2U);
    for (const Block& b : blocks) {
        if (b.kind == BlockKind::Image) {
            continue;
        }
        const int z = zone_of(b);
        if (z == 0) {
            continue;
        }
        seen[BandSigKey{z, normalized_block_text(b)}].observe(b.page);
    }

    const std::size_t repeat_threshold =
        (std::max)(kMinChromeRepeatCount, (pages.size() + 1U) / 2U);

    const auto& const_seen = seen;
    std::erase_if(blocks, [&zone_of, &const_seen, repeat_threshold](const Block& b) {
        if (b.kind == BlockKind::Image) {
            return false;
        }
        const int z = zone_of(b);
        if (z == 0) {
            return false;
        }
        std::string norm = normalized_block_text(b);
        if (is_page_number_text(norm)) {
            return true;
        }
        const auto it = const_seen.find(BandSigKey{z, std::move(norm)});
        return it != const_seen.end() && it->second.page_count >= repeat_threshold;
    });
}

}  // namespace pdf2md::layout
