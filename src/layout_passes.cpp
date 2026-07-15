#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layout_internal.h"
#include "utf8.h"

namespace pdf2md {
namespace detail {

// ---------------------------------------------------------------- doc level

double compute_body_size(const std::vector<PageData>& pages) {
    std::map<long, size_t> hist;  // size rounded to quarter points
    for (const PageData& page : pages) {
        for (const CharInfo& c : page.chars) {
            if (c.space || c.size <= 0) continue;
            hist[std::lround(c.size * 4)]++;
        }
    }
    if (hist.empty()) return 11.0;
    auto best = hist.begin();
    for (auto it = hist.begin(); it != hist.end(); ++it)
        if (it->second > best->second) best = it;
    return static_cast<double>(best->first) / 4.0;
}

std::string normalized_block_text(const Block& b) {
    std::string out;
    for (const Line& ln : b.lines) {
        for (char32_t cp : ln.plain) {
            if (is_ascii_digit(cp)) {
                if (out.empty() || out.back() != '#') out += '#';
            } else if (cp == U' ') {
                continue;
            } else if (cp < 128) {
                out += static_cast<char>(std::tolower(static_cast<int>(cp)));
            } else {
                append_utf8(out, cp);
            }
        }
        out += '|';
    }
    return out;
}

bool is_page_number_text(const std::string& norm) {
    static const char* patterns[] = {"#|", "-#-|", "page#|", "page#of#|", "#/#|", "[#]|", "|"};
    for (const char* p : patterns)
        if (norm == p) return true;
    return false;
}

// ------------------------------------------------- running chrome (line level)
//
// strip_headers_footers (below) keys repetition on whole blocks, so it misses a
// running footer whenever region_to_blocks has already fused half of it into an
// adjacent caption/reference, or whenever the XY-cut leaves the page-number and
// document-id halves on one line: the fused/merged signature is a minority and
// falls under the repeat threshold. This pass runs FIRST, at the line level
// (before any block merging), keying on per-column-segment signatures so each
// half of a footer is matched independently and a footer can never fuse with
// body text.

// Digit runs collapse to '#', spaces drop, ascii lowercases: a page-number-
// independent signature for one column segment of a line.
std::string chrome_segment_sig(const Line& ln, size_t first, size_t last) {
    std::string out;
    for (size_t i = first; i <= last && i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) continue;
        char32_t cp = c.code;
        if (is_ascii_digit(cp)) {
            if (out.empty() || out.back() != '#') out += '#';
        } else if (cp < 128) {
            out += static_cast<char>(std::tolower(static_cast<int>(cp)));
        } else {
            append_utf8(out, cp);
        }
    }
    return out;
}

// 1 = header band (glyphs wholly in the top margin), 2 = footer band, 0 = body.
// Mirrors strip_headers_footers::zone_of on a line's glyph extent (PDF space,
// y grows up).
int chrome_band_of(const Line& ln, double h) {
    if (h <= 0 || ln.chars.empty()) return 0;
    double top = -1e30, bottom = 1e30;
    for (const CharInfo& c : ln.chars) {
        top = std::max(top, c.top);
        bottom = std::min(bottom, c.bottom);
    }
    if (bottom > 0.88 * h) return 1;
    if (top < 0.12 * h) return 2;
    return 0;
}

std::vector<std::pair<size_t, size_t>> chrome_segments_of(const Line& ln) {
    std::vector<std::pair<size_t, size_t>> segs = ln.segments;
    if (segs.empty() && !ln.chars.empty()) segs.emplace_back(0, ln.chars.size() - 1);
    return segs;
}

// Position key for the watermark pass: a segment's left edge and the line's
// baseline, quantized to 4pt bins. A stamped watermark (a
// repeated DRM mark) lands at the identical coordinates on every page; repeated body wording (a
// table header used on many pages) recurs at varying heights, so keying the
// repetition on position keeps it safe.
std::pair<long, long> chrome_position_key(const Line& ln, size_t first) {
    return {std::lround(ln.chars[first].left / 4.0), std::lround(ln.baseline / 4.0)};
}

// Watermark signature: literal text (lowercased, spaces dropped), digits KEPT.
// The digit-collapsing of chrome_segment_sig exists to match footers whose page
// number varies; a stamped watermark is literally identical on every page, and
// collapsing digits here would make per-page counters ("... page 3 ...") at a
// fixed position look like repeats of each other and strip real content.
std::string chrome_watermark_sig(const Line& ln, size_t first, size_t last) {
    std::string out;
    for (size_t i = first; i <= last && i < ln.chars.size(); ++i) {
        const CharInfo& c = ln.chars[i];
        if (c.space) continue;
        char32_t cp = c.code;
        if (cp < 128) {
            out += static_cast<char>(std::tolower(static_cast<int>(cp)));
        } else {
            append_utf8(out, cp);
        }
    }
    return out;
}

// True when no other line on the page overlaps the span [x0, x1] within `dist`
// points vertically of `baseline` (the line owning the span is skipped). A
// watermark or a running head floats alone in the margin; a table header or
// body sentence has neighbouring lines right above/below, so repeated real
// content at a fixed position is never mistaken for chrome. A watermark that
// drifted onto a body line's baseline still passes: it becomes its own column
// segment there, and the body text's span does not overlap the segment's.
bool chrome_isolated_span(double x0, double x1, double baseline, const Line& self,
                          const PageLines& pg, double dist = 24.0) {
    for (const auto& region : pg.regions)
        for (const Line& other : region) {
            if (&other == &self) continue;
            if (other.x0 <= x1 && x0 <= other.x1 &&
                std::fabs(other.baseline - baseline) <= dist)
                return false;
        }
    return true;
}

// A DRM stamp lives in the page's outer horizontal margin, clear of the body
// column, typically hugging a page edge; repeated real content -- a batch
// template's centered notice, a tagline -- sits inside the column. Only
// margin-dwelling segments may qualify as (or be stripped as) watermarks.
bool chrome_in_margin(const Line& ln, size_t first, size_t last, double page_w) {
    if (page_w <= 0) return false;
    double x0 = ln.chars[first].left;
    double x1 = ln.chars[std::min(last, ln.chars.size() - 1)].right;
    return x1 < 0.15 * page_w || x0 > 0.85 * page_w;
}

void strip_running_chrome(std::vector<PageLines>& pages) {
    if (pages.size() < 3) return;  // too few pages to establish repetition

    std::map<std::pair<int, std::string>, std::set<int>> seen;  // key -> distinct pages
    // Band segments keyed by position too: a running head tied to one chapter
    // repeats on far fewer than half the pages, but always at the same corner.
    std::map<std::tuple<int, long, long, std::string>, std::set<int>> pos_seen;
    // Watermark candidates: body-band segments keyed by (position, signature).
    std::map<std::tuple<long, long, std::string>, std::set<int>> wm_seen;
    for (const PageLines& pg : pages)
        for (const auto& region : pg.regions)
            for (const Line& ln : region) {
                int band = chrome_band_of(ln, pg.height);
                for (auto [f, l] : chrome_segments_of(ln)) {
                    if (band != 0) {
                        std::string sig = chrome_segment_sig(ln, f, l);
                        if (sig.empty()) continue;
                        seen[{band, sig}].insert(pg.index);
                        auto [qx, qy] = chrome_position_key(ln, f);
                        pos_seen[{band, qx, qy, sig}].insert(pg.index);
                    } else {
                        if (!chrome_in_margin(ln, f, l, pg.width)) continue;
                        std::string sig = chrome_watermark_sig(ln, f, l);
                        if (sig.empty()) continue;
                        auto [qx, qy] = chrome_position_key(ln, f);
                        wm_seen[{qx, qy, sig}].insert(pg.index);
                    }
                }
            }

    const size_t threshold = std::max<size_t>(3, (pages.size() + 1) / 2);
    std::set<std::pair<int, std::string>> chrome;
    for (auto& [key, pset] : seen)
        if (pset.size() >= threshold) chrome.insert(key);
    // A margin-band segment whose (digit-normalized) text recurs at the SAME
    // page position on several pages is a running head even when its text is
    // chapter-scoped and so never nears the document-wide repeat threshold
    // (a chapter-scoped running head repeating on 30 of 700 pages). Real body content
    // dipping into the band never repeats position-identically 4 times.
    constexpr size_t kPosBandThreshold = 4;
    std::set<std::tuple<int, long, long, std::string>> pos_chrome;
    for (auto& [key, pset] : pos_seen)
        if (pset.size() >= kPosBandThreshold) pos_chrome.insert(key);
    // A signature qualifies as watermark text by repeating at ONE fixed page
    // position across many pages; once established, every isolated occurrence
    // of that text is stamped chrome (the stamp can sit mirrored on odd/even
    // pages, drift on a differently-trimmed cover, and share a baseline with
    // body text). A mirrored stamp splits its count across two positions, so
    // the per-position bar is a quarter of the pages, not half.
    const size_t wm_threshold = std::max<size_t>(3, (pages.size() + 3) / 4);
    std::set<std::string> watermark_sigs;
    for (auto& [key, pset] : wm_seen) {
        // One-char signatures (a stray digit, a list bullet) are too generic to
        // treat as watermark text, and anything longer than a few words is
        // template content, not a stamp.
        const std::string& sig = std::get<2>(key);
        if (sig.size() < 2 || sig.size() > 24) continue;
        if (pset.size() >= wm_threshold) watermark_sigs.insert(sig);
    }
    static const bool debug_wm = std::getenv("PDF2MD_DEBUG_WM") != nullptr;
    if (debug_wm)
        for (const auto& s : watermark_sigs)
            std::fprintf(stderr, "WM sig %s\n", s.c_str());

    // Watermark removal is per column segment: a stamp that landed on a body
    // line's baseline was fused into that line as a trailing segment, so the
    // whole line must survive minus the stamp's glyphs. Lines losing every
    // glyph are dropped; partially stripped ones are re-finalized.
    if (!watermark_sigs.empty()) {
        for (PageLines& pg : pages)
            for (auto& region : pg.regions) {
                for (Line& ln : region) {
                    if (chrome_band_of(ln, pg.height) != 0) continue;
                    std::vector<std::pair<size_t, size_t>> drop;
                    for (auto [f, l] : chrome_segments_of(ln)) {
                        if (!chrome_in_margin(ln, f, l, pg.width)) continue;
                        std::string sig = chrome_watermark_sig(ln, f, l);
                        if (sig.empty() || !watermark_sigs.count(sig)) continue;
                        if (!chrome_isolated_span(ln.chars[f].left, ln.chars[l].right,
                                                  ln.baseline, ln, pg))
                            continue;
                        drop.emplace_back(f, l);
                    }
                    if (drop.empty()) continue;
                    std::vector<CharInfo> kept;
                    kept.reserve(ln.chars.size());
                    for (size_t i = 0; i < ln.chars.size(); ++i) {
                        bool in_drop = false;
                        for (auto [f, l] : drop)
                            if (i >= f && i <= l) { in_drop = true; break; }
                        if (!in_drop) kept.push_back(ln.chars[i]);
                    }
                    ln.chars = std::move(kept);
                    bool has_text = std::any_of(ln.chars.begin(), ln.chars.end(),
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

    if (chrome.empty() && pos_chrome.empty()) return;

    // Drop a band line only when every one of its column segments is a known
    // chrome signature (document-wide by text, or position-anchored): a footer
    // split across two segments (page-number + document-id) is removed whole,
    // while a real body line that merely dips into the margin keeps at least
    // one non-repeating segment and survives. A position-anchored match (the
    // lower 4-page bar) additionally requires the line to float clear of its
    // neighbours: a borderless table whose header row repeats at the top of
    // several pages has data rows right beneath it and must keep its labels,
    // while a genuine running head sits alone in the margin.
    for (PageLines& pg : pages)
        for (auto& region : pg.regions)
            std::erase_if(region, [&](const Line& ln) {
                int band = chrome_band_of(ln, pg.height);
                if (band == 0) return false;
                bool any = false;
                for (auto [f, l] : chrome_segments_of(ln)) {
                    std::string sig = chrome_segment_sig(ln, f, l);
                    if (sig.empty()) continue;
                    if (!chrome.count({band, sig})) {
                        auto [qx, qy] = chrome_position_key(ln, f);
                        if (!pos_chrome.count({band, qx, qy, sig})) return false;
                        double dist = std::max(1.8 * ln.size, 14.0);
                        if (!chrome_isolated_span(ln.chars[f].left, ln.chars[l].right,
                                                  ln.baseline, ln, pg, dist))
                            return false;
                    }
                    any = true;
                }
                return any;
            });
}

void strip_headers_footers(std::vector<Block>& blocks, const std::vector<PageData>& pages) {
    if (pages.size() < 2) return;
    std::unordered_map<int, std::pair<double, double>> page_dims;
    for (const PageData& p : pages) page_dims[p.index] = {p.width, p.height};

    auto zone_of = [&](const Block& b) -> int {
        auto it = page_dims.find(b.page);
        if (it == page_dims.end()) return 0;
        double h = it->second.second;
        if (h <= 0) return 0;
        if (b.y_bottom > 0.88 * h) return 1;   // header zone
        if (b.y_top < 0.12 * h) return 2;      // footer zone
        return 0;
    };

    // Count normalized text occurrences per zone across distinct pages.
    std::map<std::pair<int, std::string>, std::map<int, int>> seen;
    for (const Block& b : blocks) {
        if (b.kind == BlockKind::Image) continue;
        int z = zone_of(b);
        if (z == 0) continue;
        seen[{z, normalized_block_text(b)}][b.page]++;
    }

    const size_t page_count = pages.size();
    const size_t repeat_threshold =
        std::max<size_t>(3, (page_count + 1) / 2);

    std::erase_if(blocks, [&](const Block& b) {
        if (b.kind == BlockKind::Image) return false;
        int z = zone_of(b);
        if (z == 0) return false;
        std::string norm = normalized_block_text(b);
        if (is_page_number_text(norm)) return true;
        auto it = seen.find({z, norm});
        return it != seen.end() && it->second.size() >= repeat_threshold;
    });
}

// Depth of a leading dotted section number in a heading ("3.2.3 ..." -> 3),
// 0 when the heading is not numbered. Used to nest sub-subsections that share a
// font size with their parent.
int heading_number_depth(const Block& b) {
    if (b.lines.empty()) return 0;
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') ++i;
    if (i >= s.size() || !is_ascii_digit(s[i])) return 0;
    int depth = 0;
    while (i < s.size() && is_ascii_digit(s[i])) {
        while (i < s.size() && is_ascii_digit(s[i])) ++i;  // one numeric component
        ++depth;
        if (i + 1 < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1]))
            ++i;  // dotted separator to the next component
        else
            break;
    }
    // The number must be a section label (followed by a separator/end), not the
    // first digits of a sentence like "512-dimensional".
    if (i < s.size() && s[i] != U' ' && s[i] != U'.') return 0;
    return depth;
}

// Parses a leading hierarchical section number: an optional single capital-letter
// appendix component ("E."), then one or more dotted numeric components ("11.4"),
// which must be followed by a space and title text. Returns the canonical dotted
// path ("E.11.4") and its depth (component count), or depth 0 when the line does
// not open with such a number. This recognizes both body sections ("8.42.1") and
// appendix sections ("C.1.1") under one scheme.
struct SectionNumber {
    std::u32string canon;
    int depth = 0;
};
SectionNumber parse_section_number(const std::u32string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') ++i;
    std::u32string canon;
    int depth = 0;
    // Optional appendix-letter component: a single A-Z, a dot, then a digit.
    if (i + 2 < s.size() && is_ascii_upper(s[i]) && s[i + 1] == U'.' &&
        is_ascii_digit(s[i + 2])) {
        canon += s[i];
        ++depth;
        i += 2;  // step over the letter and its dot
    }
    bool have_number = false;
    while (i < s.size() && is_ascii_digit(s[i])) {
        if (!canon.empty()) canon += U'.';
        while (i < s.size() && is_ascii_digit(s[i])) { canon += s[i]; ++i; }
        ++depth;
        have_number = true;
        if (i + 1 < s.size() && s[i] == U'.' && is_ascii_digit(s[i + 1]))
            ++i;  // dotted separator to the next component
        else
            break;
    }
    if (!have_number) return {};                    // needs at least one numeric part
    if (i >= s.size() || s[i] != U' ') return {};   // number must end the label
    size_t t = i;
    while (t < s.size() && s[t] == U' ') ++t;
    if (t >= s.size()) return {};                   // title text must follow
    return {canon, depth};
}

// Normalized comparison key for a block's text: ASCII letters/digits lowercased,
// every other run collapsed to a single space. Two candidates sharing a key are
// the same label; a label that recurs is a repeated structural element (a table
// header, a form field), never a unique section title.
std::string heading_norm_key(const Block& b) {
    std::string k;
    bool gap = false;
    for (const Line& ln : b.lines) {
        for (char32_t c : ln.plain) {
            if (is_ascii_upper(c) || is_ascii_lower(c) || is_ascii_digit(c)) {
                if (gap && !k.empty()) k += ' ';
                k += static_cast<char>(is_ascii_upper(c) ? c - U'A' + U'a' : c);
                gap = false;
            } else {
                gap = true;
            }
        }
        gap = true;
    }
    return k;
}

// If the first line reads as a "Label: value" metadata field, return the label
// (text before the first field-ending colon); else "". A scoped identifier
// ("app::util") never qualifies because its colons are not followed by a space,
// and a title colon inside a long clause is excluded by the short-label cap.
std::string heading_meta_label(const Block& b) {
    if (b.lines.empty()) return {};
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') ++i;
    std::string label;
    bool has_letter = false;
    for (; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c == U':') {
            if (i + 1 < s.size() && s[i + 1] != U' ') return {};  // "::" / "a:b"
            break;                                                // field-ending colon
        }
        if (is_letter_cp(c)) has_letter = true;
        else if (!is_ascii_digit(c) && c != U' ' && c != U'-' && c != U'_' &&
                 c != U'/' && c != U'(' && c != U')' && c != U'&' && c != U',')
            return {};                                            // not a plain key
        if (c < 128) label += static_cast<char>(c);
        if (label.size() > 40) return {};                         // labels are short
    }
    if (i >= s.size() || !has_letter) return {};                  // no colon / no letters
    while (!label.empty() && label.back() == ' ') label.pop_back();
    return label;
}

// A figure/table caption: the caption-label word, a number, then a separator.
// Keyed on the typographic convention shared by technical documents, not on any
// document-specific string.
bool heading_is_caption(const Block& b) {
    if (b.lines.empty()) return false;
    const std::u32string& s = b.lines.front().plain;
    size_t i = 0;
    while (i < s.size() && s[i] == U' ') ++i;
    auto matches = [&](const char32_t* w) {
        size_t k = i;
        for (; *w; ++w, ++k)
            if (k >= s.size() || s[k] != *w) return false;
        return k < s.size() && s[k] == U' ' && k + 1 < s.size() && is_ascii_digit(s[k + 1]);
    };
    return matches(U"Figure") || matches(U"Table");
}

void classify_headings(std::vector<Block>& blocks, double body_size) {
    // Total characters per candidate size tier; oversized tiers are body text.
    size_t total_chars = 0;
    for (const Block& b : blocks)
        for (const Line& ln : b.lines) total_chars += ln.plain.size();

    auto tier_key = [](double size) { return std::lround(size * 2); };  // half-point buckets

    std::map<long, size_t, std::greater<long>> tier_chars;
    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph) continue;
        if (b.lines.size() > 3) continue;
        size_t len = 0;
        for (const Line& ln : b.lines) len += ln.plain.size();
        if (len == 0 || len > 220) continue;
        if (b.size >= body_size * 1.12) tier_chars[tier_key(b.size)] += len;
    }
    // Drop tiers that cover too much of the document to be headings.
    std::vector<long> tiers;
    for (auto& [key, chars] : tier_chars) {
        if (total_chars == 0 || chars * 100 <= total_chars * 30) tiers.push_back(key);
    }

    auto level_of = [&](double size) -> int {
        long key = tier_key(size);
        for (size_t i = 0; i < tiers.size(); ++i)
            if (tiers[i] == key) return static_cast<int>(std::min<size_t>(i + 1, 6));
        return 0;
    };

    // Basic gate shared by both promotion branches: a short, non-empty block that
    // is heading-sized or a bold body-sized single line.
    auto size_tier_cand = [&](const Block& b, size_t len) {
        return b.lines.size() <= 3 && len > 0 && len <= 220 && b.size >= body_size * 1.12;
    };
    auto bold_line_cand = [&](const Block& b, size_t len) {
        return b.lines.size() == 1 && b.bold_majority && len <= 90 && len > 0 &&
               b.size >= body_size * 0.98 && b.size < body_size * 1.12;
    };

    // Frequency of each candidate's normalized text and, where present, its
    // metadata-field label. A candidate whose label or full text recurs across
    // the document is a repeated form field / structural cell, not a section
    // title. Counts are taken over candidate blocks only.
    std::unordered_map<std::string, int> full_freq, label_freq;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || b.lines.empty()) continue;
        size_t len = 0;
        for (const Line& ln : b.lines) len += ln.plain.size();
        if (!size_tier_cand(b, len) && !bold_line_cand(b, len)) continue;
        ++full_freq[heading_norm_key(b)];
        std::string lab = heading_meta_label(b);
        if (!lab.empty()) ++label_freq[lab];
    }

    // Robust body-column edges (10th/90th percentile over multi-line paragraphs)
    // used to recognize a genuine inset callout for the blockquote fallback.
    double body_x0 = 0, body_x1 = 0;
    {
        std::vector<double> xs0, xs1;
        for (const Block& b : blocks)
            if (b.kind == BlockKind::Paragraph && b.lines.size() >= 2) {
                xs0.push_back(b.x0);
                xs1.push_back(b.x1);
            }
        if (!xs0.empty()) {
            std::sort(xs0.begin(), xs0.end());
            std::sort(xs1.begin(), xs1.end());
            body_x0 = xs0[xs0.size() / 10];
            body_x1 = xs1[xs1.size() * 9 / 10];
        }
    }
    double body_w = body_x1 - body_x0;
    auto is_inset_callout = [&](const Block& b) {
        return body_w > 0 && (b.x0 - body_x0) >= 0.02 * body_w &&
               (body_x1 - b.x1) >= 0.02 * body_w;
    };

    // Genuine section-title evidence: not running prose (no sentence/list-ending
    // punctuation), not a bracketed reference entry, not a figure/table caption,
    // not a repeated metadata field or structural label.
    auto titleish = [&](const Block& b) -> bool {
        if (b.lines.empty()) return false;
        const std::u32string& lastln = b.lines.back().plain;
        size_t e = lastln.size();
        while (e > 0 && lastln[e - 1] == U' ') --e;
        if (e > 0) {
            char32_t last = lastln[e - 1];
            if (last == U'.' || last == U',' || last == U';' || last == U':' ||
                last == U'!' || last == U'?')
                return false;
        }
        if (is_citation_lead(b.lines.front())) return false;
        if (heading_is_caption(b)) return false;
        std::string lab = heading_meta_label(b);
        if (!lab.empty() && label_freq[lab] >= 3) return false;
        if (full_freq[heading_norm_key(b)] >= 3) return false;
        return true;
    };

    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph) continue;
        size_t len = 0;
        for (const Line& ln : b.lines) len += ln.plain.size();

        if (size_tier_cand(b, len)) {
            int lvl = level_of(b.size);
            if (lvl > 0) {
                if (titleish(b)) {
                    b.kind = BlockKind::Heading;
                    b.heading_level = lvl;
                } else {
                    // Not a section title. Demote to a blockquote only for a
                    // sentence-terminated multi-line block carrying real quote
                    // geometry (inset from the body column on both sides, e.g. a
                    // centred legal notice); otherwise it is ordinary prose that
                    // merely measured above body size and stays a plain paragraph.
                    char32_t last =
                        b.lines.back().plain.empty() ? U'\0' : b.lines.back().plain.back();
                    bool sentence = last == U'.' || last == U'!' || last == U'?';
                    if (b.lines.size() >= 2 && sentence && is_inset_callout(b))
                        b.blockquote = true;
                }
                continue;
            }
        }
        // Bold, body-sized single lines read as the lowest heading tier.
        if (bold_line_cand(b, len) && titleish(b)) {
            b.kind = BlockKind::Heading;
            b.heading_level = static_cast<int>(std::min<size_t>(tiers.size() + 1, 6));
            if (b.heading_level < 2) b.heading_level = 2;
        }
    }

    // Derive heading depth from a leading dotted section number so a
    // sub-subsection (3.2.3) nests one level below its parent (3.2) even when the
    // two share a font size. Anchor the mapping on the shallowest numbered
    // heading's already-assigned level, so a document that starts numbering at a
    // deeper level (or offsets the top section) still nests consistently.
    int anchor_depth = 0, anchor_level = 0;
    bool have_anchor = false;
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading) continue;
        int d = heading_number_depth(b);
        if (d == 0) continue;
        if (!have_anchor || d < anchor_depth ||
            (d == anchor_depth && b.heading_level < anchor_level)) {
            anchor_depth = d;
            anchor_level = b.heading_level;
            have_anchor = true;
        }
    }
    if (have_anchor) {
        int base = anchor_level - (anchor_depth - 1);
        for (Block& b : blocks) {
            if (b.kind != BlockKind::Heading) continue;
            int d = heading_number_depth(b);
            if (d == 0) continue;
            b.heading_level = std::clamp(base + (d - 1), 1, 6);
        }
    }

    // Numbered run-in titles. A short, bold, body-sized line that opens with a
    // dotted section number exactly one component deeper than an enclosing heading
    // of the same numbering scheme is itself a section heading, even at body size
    // -- the typographic convention that sets deep subsection titles as bold
    // run-ins rather than enlarged headings. Anchored on the headings already
    // classified above (a snapshot), so a promotion never becomes a parent for a
    // still-deeper line: this can only add the one level directly below an existing
    // heading, never cascade a whole numeric subtree into ever-deeper headings.
    std::unordered_map<std::u32string, int> heading_number_level;  // canon -> level
    for (const Block& b : blocks) {
        if (b.kind != BlockKind::Heading || b.lines.empty()) continue;
        SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth == 0) continue;
        auto it = heading_number_level.find(sn.canon);
        if (it == heading_number_level.end() || b.heading_level < it->second)
            heading_number_level[sn.canon] = b.heading_level;
    }
    for (Block& b : blocks) {
        if (b.kind != BlockKind::Paragraph || !b.bold_majority || b.lines.size() != 1) continue;
        size_t len = b.lines.front().plain.size();
        if (len == 0 || len > 140) continue;
        SectionNumber sn = parse_section_number(b.lines.front().plain);
        if (sn.depth < 2) continue;
        size_t dot = sn.canon.find_last_of(U'.');
        if (dot == std::u32string::npos) continue;
        auto it = heading_number_level.find(sn.canon.substr(0, dot));  // parent prefix
        if (it == heading_number_level.end()) continue;
        if (!titleish(b)) continue;
        b.kind = BlockKind::Heading;
        b.heading_level = std::clamp(it->second + 1, 1, 6);
    }
}

// Numbered footnotes float to the end of the document as markdown footnote
// definitions ("[^n]: ..."), so they no longer interrupt the body paragraph they
// were rendered beneath (which would otherwise strand a cross-page sentence).
void relocate_footnotes(std::vector<Block>& blocks) {
    int last_page = 0;
    for (const Block& b : blocks) last_page = std::max(last_page, b.page);
    std::vector<Block> kept, notes;
    kept.reserve(blocks.size());
    for (Block& b : blocks) {
        if (b.is_footnote && b.footnote_numbered) {
            b.page = last_page;  // keep them at the end for page-break output too
            notes.push_back(std::move(b));
        } else {
            kept.push_back(std::move(b));
        }
    }
    for (Block& n : notes) kept.push_back(std::move(n));
    blocks = std::move(kept);
}

// A one-line paragraph whose whole content is a single math atom carrying only
// punctuation around it (e.g. "$PE_{pos}$ ."). Such a block is the tail of the
// sentence above, stranded when the wrap left a lone formula on its own line.
bool is_lone_math_atom(const Block& b) {
    if (b.kind != BlockKind::Paragraph || b.is_footnote || b.blockquote) return false;
    if (b.lines.size() != 1) return false;
    bool has_math = false;
    for (const StyledRun& r : b.lines.front().runs) {
        if (r.math) { has_math = true; continue; }
        for (char32_t c : r.text) {
            if (c == U' ') continue;
            if (is_letter_cp(c) || (c >= U'0' && c <= U'9')) return false;  // real prose
        }
    }
    return has_math;
}

// Rejoins such a stranded math atom with the paragraph it belongs to: the
// previous block must be a paragraph whose last line stops mid-sentence (no
// terminator), so the atom clearly continues it.
void merge_trailing_math_atoms(std::vector<Block>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.kind != BlockKind::Paragraph || prev.is_footnote) continue;
        if (prev.lines.empty() || cur.lines.empty()) continue;
        if (!is_lone_math_atom(cur)) continue;
        if (std::fabs(prev.size - cur.size) > 0.2 * std::max(prev.size, cur.size)) continue;
        const std::u32string& ptext = prev.lines.back().plain;
        size_t e = ptext.size();
        while (e > 0 && ptext[e - 1] == U' ') --e;
        if (e == 0) continue;
        char32_t last = ptext[e - 1];
        // Only an unterminated sentence continues into the atom: a letter, digit,
        // comma or wrap hyphen dangles; a full stop / colon / closer does not.
        bool continues = is_letter_cp(last) || (last >= U'0' && last <= U'9') ||
                         last == U',' || last == U'-' || last == 0x2010 || last == 0x00AD;
        if (!continues) continue;

        for (Line& ln : cur.lines) prev.lines.push_back(std::move(ln));
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

// A block whose entire content is table-continuation marker glyphs -- the small
// hollow triangles a ruled table prints at the bottom (continues below, U+25BD)
// and top (continued from above, U+25B3) of a page split. Geometry-free
// repetition keys, not document strings.
bool is_table_continuation_marker(const Block& b) {
    if (b.kind != BlockKind::Paragraph || b.lines.empty()) return false;
    bool any = false;
    for (const Line& ln : b.lines)
        for (char32_t c : ln.plain) {
            if (c == U' ') continue;
            if (c != 0x25BD && c != 0x25B3) return false;
            any = true;
        }
    return any;
}

// Two ruled tables share a column signature when they have the same number of
// column boundaries at (nearly) the same x-positions -- the fingerprint of one
// table split across a page break.
bool same_column_signature(const Block& a, const Block& b) {
    if (a.col_edges.empty() || a.col_edges.size() != b.col_edges.size()) return false;
    const double tol = std::max(3.0, 0.4 * std::max(a.size, 1.0));
    for (size_t i = 0; i < a.col_edges.size(); ++i)
        if (std::fabs(a.col_edges[i] - b.col_edges[i]) > tol) return false;
    return true;
}

// A table row that reads as a header: a solid majority of its non-empty cells are
// wholly bold. A continuation fragment that repeats no header instead opens with a
// plain data row, so it must be stitched onto the previous fragment.
bool table_row_is_header(const std::vector<TableCell>& row) {
    size_t nonempty = 0, bold = 0;
    for (const TableCell& cell : row) {
        bool has_ink = false, all_bold = true;
        for (const StyledRun& r : cell)
            for (char32_t c : r.text)
                if (c != U' ') { has_ink = true; if (!r.bold) all_bold = false; }
        if (!has_ink) continue;
        ++nonempty;
        if (all_bold) ++bold;
    }
    return nonempty > 0 && bold * 2 >= nonempty;
}

// Stitches a ruled table that a page break split into per-page fragments back
// into one table. A later fragment on the next page with the same column
// signature is a continuation when either a continuation-marker glyph sits
// between the fragments, or the later fragment repeats no header (it opens with a
// data row) while the earlier one is headed. The continuation's rows append to the
// first fragment and the marker blocks between them are dropped.
void merge_ruled_table_continuations(std::vector<Block>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].kind != BlockKind::Table || blocks[i].col_edges.empty()) continue;
        bool prev_header = !blocks[i].table_cells.empty() &&
                           table_row_is_header(blocks[i].table_cells.front());
        int last_page = blocks[i].page;
        while (true) {
            size_t j = i + 1;
            bool marker_between = false;
            while (j < blocks.size() && is_table_continuation_marker(blocks[j])) {
                marker_between = true;
                ++j;
            }
            if (j >= blocks.size()) break;
            Block& later = blocks[j];
            if (later.kind != BlockKind::Table || later.col_edges.empty()) break;
            if (later.page != last_page + 1) break;
            if (!same_column_signature(blocks[i], later)) break;
            bool later_header = !later.table_cells.empty() &&
                                table_row_is_header(later.table_cells.front());
            if (!(marker_between || (prev_header && !later_header))) break;

            for (auto& row : later.table_cells) blocks[i].table_cells.push_back(std::move(row));
            blocks[i].y_bottom = later.y_bottom;
            last_page = later.page;
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                         blocks.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        }
    }
}

// The trailing gap-delimited token of a line when it is a run of ASCII digits set
// off from the preceding ink by at least a word gap -- the shape of a
// table-of-contents page number. Returns the char index at which the number
// begins, or -1 when the line does not end in such a set-off integer.
long trailing_page_number(const Line& ln) {
    long hi = -1;
    for (long i = static_cast<long>(ln.chars.size()) - 1; i >= 0; --i)
        if (!ln.chars[i].space) { hi = i; break; }
    if (hi < 0 || ln.chars[hi].code < U'0' || ln.chars[hi].code > U'9') return -1;
    long lo = hi;
    while (lo > 0 && !ln.chars[lo - 1].space && ln.chars[lo - 1].code >= U'0' &&
           ln.chars[lo - 1].code <= U'9')
        --lo;
    long p = lo - 1;
    while (p >= 0 && ln.chars[p].space) --p;
    if (p < 0) return -1;  // the line holds nothing but the number
    const double wgap = ln.word_gap > 0 ? ln.word_gap : 0.28 * ln.size;
    bool space_between = false;
    for (long k = p + 1; k < lo; ++k)
        if (ln.chars[k].space) space_between = true;
    if (!space_between && ln.chars[lo].left - ln.chars[p].right < wgap) return -1;
    return lo;
}

// Folds a table-of-contents entry that escaped its table back in. An entry that
// runs long enough to leave no dot leader before its right-aligned page number
// carries a single column segment: it fails the multi-segment table test and
// surfaces as a bare paragraph wedged between the rows of the two-column table it
// belongs to. Such an orphan is recognized purely by geometry -- one
// single-segment line whose trailing token is an integer right-aligned to the
// preceding table's page-number column, its entry starting within that table's
// body and set flush against its last row -- and folded in as a row. When a
// matching table half follows (the lower part of the same split), the two halves
// stitch into one continuous table with the orphan between them.
void fold_toc_orphan_rows(std::vector<Block>& blocks) {
    auto two_col_toc_table = [](const Block& b) {
        if (b.kind != BlockKind::Table || !b.col_edges.empty() || b.table_cells.empty())
            return false;
        size_t maxcol = 0;
        for (const auto& row : b.table_cells) maxcol = std::max(maxcol, row.size());
        return maxcol == 2;
    };
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& P = blocks[i];
        if (P.kind != BlockKind::Paragraph || P.lines.size() != 1) continue;
        const Line& ln = P.lines[0];
        if (ln.segments.size() >= 2) continue;  // a dot-leader row already splits
        Block& T1 = blocks[i - 1];
        if (!two_col_toc_table(T1) || T1.page != P.page) continue;
        long ns = trailing_page_number(ln);
        if (ns <= 0) continue;
        const double tol = std::max(3.0, 0.5 * std::max(T1.size, 1.0));
        const double vgap = 2.5 * std::max(T1.size, 1.0);
        // The number lands right-aligned in the table's page-number column, the
        // entry starts within the table body, and the line sits flush below it.
        if (std::fabs(P.x1 - T1.x1) > tol) continue;
        if (P.x0 < T1.x0 - tol) continue;
        if (T1.y_bottom - P.y_top > vgap) continue;
        std::vector<TableCell> row(2);
        row[0] = render_cell_runs(ln, 0, static_cast<size_t>(ns) - 1);
        row[1] = render_cell_runs(ln, static_cast<size_t>(ns), ln.chars.size() - 1);
        if (row[0].empty() || row[1].empty()) continue;
        T1.table_cells.push_back(std::move(row));
        T1.x1 = std::max(T1.x1, P.x1);
        T1.y_bottom = std::min(T1.y_bottom, P.y_bottom);
        std::ptrdiff_t erase_end = static_cast<std::ptrdiff_t>(i) + 1;  // exclusive
        if (i + 1 < blocks.size()) {
            Block& T2 = blocks[i + 1];
            // The lower half of the same split: a two-column table sharing the
            // page-number column, set flush below the orphan.
            if (two_col_toc_table(T2) && T2.page == P.page &&
                std::fabs(T2.x1 - T1.x1) <= tol && T2.x0 >= T1.x0 - tol &&
                P.y_bottom - T2.y_top <= vgap) {
                for (auto& r : T2.table_cells) T1.table_cells.push_back(std::move(r));
                T1.x0 = std::min(T1.x0, T2.x0);
                T1.x1 = std::max(T1.x1, T2.x1);
                T1.y_bottom = std::min(T1.y_bottom, T2.y_bottom);
                erase_end = static_cast<std::ptrdiff_t>(i) + 2;
            }
        }
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i),
                     blocks.begin() + erase_end);
        --i;  // re-examine from the grown table for a chained orphan
    }
}

// Rejoins the wrapped lines of a bracketed reference entry ("[12] ISO ... –
// Part 3: ..." / a trailing URL) that the block splitter or a page break left as
// separate paragraphs. A reference is set with a hanging indent, so a fragment
// following an entry whose last line did not end a sentence is that entry's
// continuation -- unless it opens a new "[n]" entry. Merges continuation fragments
// (including a lone URL) forward into the entry, within the same reference run.
void merge_reference_entries(std::vector<Block>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.kind != BlockKind::Paragraph || prev.is_footnote || prev.blockquote) continue;
        if (cur.kind != BlockKind::Paragraph || cur.is_footnote || cur.blockquote) continue;
        if (prev.lines.empty() || cur.lines.empty()) continue;
        // The chain is anchored on a bracketed citation entry; a continuation that
        // itself opens a new "[n]" entry ends the previous one.
        if (!is_citation_lead(prev.lines.front())) continue;
        if (is_citation_lead(cur.lines.front())) continue;
        // A continuation stays within the reference run: same page, or the next
        // page top (a page-straddling entry). Compatible body size.
        if (cur.page != prev.page && cur.page != prev.page + 1) continue;
        if (std::fabs(prev.size - cur.size) > 0.15 * std::max(prev.size, cur.size)) continue;
        // Only an unterminated entry continues: its last visible line stops
        // mid-field (no sentence terminator), so the next fragment is its tail.
        const std::u32string& ptext = prev.lines.back().plain;
        size_t e = ptext.size();
        while (e > 0 && ptext[e - 1] == U' ') --e;
        if (e == 0) continue;
        char32_t last = ptext[e - 1];
        if (last == U'.' || last == U'!' || last == U'?') continue;

        for (Line& ln : cur.lines) prev.lines.push_back(std::move(ln));
        finalize_block_geometry(prev);
        blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        --i;
    }
}

void merge_cross_page_paragraphs(std::vector<Block>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        Block& prev = blocks[i - 1];
        Block& cur = blocks[i];
        if (prev.page + 1 != cur.page) continue;
        if (prev.kind != BlockKind::Paragraph || cur.kind != BlockKind::Paragraph) continue;
        if (prev.is_footnote || cur.is_footnote) continue;  // notes never chain into prose
        if (prev.lines.empty() || cur.lines.empty()) continue;
        if (std::fabs(prev.size - cur.size) > 0.08 * std::max(prev.size, cur.size)) continue;

        const std::u32string& ptext = prev.lines.back().plain;
        const std::u32string& ctext = cur.lines.front().plain;
        if (ptext.empty() || ctext.empty()) continue;
        char32_t last = ptext.back();
        char32_t first = ctext.front();
        // A trailing "::" is a C++-style scope operator, not a sentence colon: its
        // member (which legitimately starts uppercase, "app::util::Event") is the
        // continuation, so the page-straddling identifier rejoins.
        bool scoped = last == U':' && ptext.size() >= 2 &&
                      ptext[ptext.size() - 2] == U':';
        static const std::u32string enders = U".!?:;”’\"')]";
        bool continues = scoped
                             ? (is_ascii_alpha(first) || is_ascii_digit(first) ||
                                first == U'_')
                             : (enders.find(last) == std::u32string::npos) &&
                                   (is_ascii_lower(first) || last == U'-' || last == 0x00AD);
        if (!continues) continue;

        for (Line& ln : cur.lines) prev.lines.push_back(std::move(ln));
        finalize_block_geometry(prev);
        prev.page = cur.page;  // allow chaining across three or more pages
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

            // Place the figure just above its caption. Anchoring on the region's
            // vertical centre keeps text that sits beside the figure's top (a
            // section heading) above it while the caption below stays below it:
            // insert before the nearest same-page block that starts below the
            // centre. Falls back to the next page's first block, then the end.
            const double anchor = (img.y_top + img.y_bottom) / 2.0;
            auto pos = blocks.end();
            auto best = blocks.end();
            double best_top = -1e30;
            for (auto it = blocks.begin(); it != blocks.end(); ++it) {
                if (it->page != page.index) {
                    if (it->page > page.index && pos == blocks.end()) pos = it;
                    continue;
                }
                if (it->y_top < anchor && it->y_top > best_top) {
                    best_top = it->y_top;
                    best = it;
                }
            }
            if (best != blocks.end()) pos = best;
            blocks.insert(pos, std::move(b));
        }
    }
}

// ------------------------------------------------------------ pass pipeline
//
// The document-level cleanup that runs after per-page assembly is a fixed
// sequence of independent transforms over doc.blocks. Each transform is a
// *Strategy*: an interchangeable member of one algorithm family (DocumentPass),
// all sharing a single interface so the driver can hold them uniformly and the
// factory can order them. DocumentPass::run is a *Template Method*: the
// enabled()-gate (the per-param `if (...)` that used to sit in analyze_layout)
// is the invariant skeleton, while apply() is the one varying step each concrete
// strategy supplies. This pulls the gates out of both analyze_layout and the
// passes' bodies, leaving each pass a thin delegation to its free function.

class MergeRuledTableContinuationsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        merge_ruled_table_continuations(doc.blocks);
    }
};

class FoldTocOrphanRowsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const override {
        return params.detect_tables;
    }
    void apply(Document& doc, const PassContext&) const override {
        fold_toc_orphan_rows(doc.blocks);
    }
};

class StripHeadersFootersPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const override {
        return params.strip_headers_footers;
    }
    void apply(Document& doc, const PassContext& ctx) const override {
        strip_headers_footers(doc.blocks, ctx.pages);
    }
};

class ClassifyHeadingsPass final : public DocumentPass {
protected:
    bool enabled(const AnalyzeParams& params) const override {
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

// Factory Method: assembles the document-cleanup pipeline. THE ORDER IS A
// CONTRACT. Several passes depend on running before or after another, and the
// output stays byte-identical only when this exact sequence is preserved.
std::vector<std::unique_ptr<DocumentPass>> build_pass_pipeline() {
    std::vector<std::unique_ptr<DocumentPass>> passes;
    // Stitch a ruled table split across a page break back into one table before
    // the chrome stripper runs (it keys on repeated blocks and would otherwise see
    // the continuation-marker glyphs as running chrome).
    passes.push_back(std::make_unique<MergeRuledTableContinuationsPass>());
    // Fold a dot-leaderless table-of-contents entry that dropped out between the
    // rows of its two-column table back in as a row (before headings classify it).
    passes.push_back(std::make_unique<FoldTocOrphanRowsPass>());
    // Safety net for any multi-line/whole-block chrome the line pass leaves.
    passes.push_back(std::make_unique<StripHeadersFootersPass>());
    passes.push_back(std::make_unique<ClassifyHeadingsPass>());
    // Relocate footnotes before the cross-page merge, so it sees adjacency.
    passes.push_back(std::make_unique<RelocateFootnotesPass>());
    passes.push_back(std::make_unique<MergeTrailingMathAtomsPass>());
    passes.push_back(std::make_unique<MergeReferenceEntriesPass>());
    passes.push_back(std::make_unique<MergeCrossPageParagraphsPass>());
    passes.push_back(std::make_unique<InsertImageBlocksPass>());
    return passes;
}

}  // namespace detail
}  // namespace pdf2md
