#include "pdf2md/pdf/figures.h"

#include "pdf2md/pdf/pdfium_raii.h"
#include "pdf2md/utf8.h"

#include <fpdf_edit.h>
#include <fpdf_structtree.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf2md::pdf {

using model::Interval;
using model::Rect;
using model::has_env_flag;
using model::rects_close;

namespace {

constexpr float kMinObjectExtentPt = 4.0F;
constexpr double kBgMinSpanFraction = 0.60;
constexpr double kBgMinAreaFraction = 0.55;
constexpr std::size_t kBgMinOcrGlyphs = 25U;

constexpr double kProseEdgeInsetRatio = 0.15;
constexpr double kProseMaxEdgeInsetPt = 40.0;
constexpr std::size_t kMinInteriorProseGlyphs = 25U;

constexpr double kBaselineClusterRatio = 0.6;
constexpr int kMinProseLineChars = 24;
constexpr double kMinProseWidthFraction = 0.5;
constexpr double kMinProseFillFraction = 0.60;
constexpr double kMaxProseGapRatio = 1.5;

constexpr int kMaxFormNestingDepth = 8;
constexpr int kMinDiagramVectorPaths = 12;
constexpr double kMaxDiagramFormScale = 0.90;

constexpr double kFigureClusterGapPt = 18.0;
constexpr double kMaxFigureOvershootRatio = 1.3;
constexpr double kMinFigureWidthRatio = 0.12;
constexpr double kMinFigureHeightRatio = 0.08;

constexpr double kClipEdgeBandRatio = 0.25;
constexpr double kClipMaxEdgeBandPt = 80.0;
constexpr double kClipGlyphClearancePt = 2.0;
constexpr double kMinClippedHeightPt = 4.0;

constexpr double kRegionRenderScale = 2.0;  // ~144 DPI
constexpr int kMaxRenderDimPx = 8000;
constexpr unsigned long kOpaqueWhiteArgb = 0xFFFFFFFFUL;

constexpr double kMinDiagramBandHeight = 40.0;
constexpr double kMinBandInkFraction = 0.004;
constexpr double kOuterMarginStampRatio = 0.90;
constexpr double kBandGapInsetPt = 2.0;
constexpr double kMinTrimmedBandRatio = 0.6;
constexpr int kScanBandPadPx = 12;

bool is_page_background_image(float left, float bottom, float right, float top,
                              const PageData& page) {
    static const bool debug_bg = has_env_flag("PDF2MD_DEBUG_BG");
    const double on_page_w = (std::min)(static_cast<double>(right), page.width) -
                             (std::max)(static_cast<double>(left), 0.0);
    const double on_page_h = (std::min)(static_cast<double>(top), page.height) -
                             (std::max)(static_cast<double>(bottom), 0.0);
    std::size_t glyphs_on_top = 0U;
    for (const CharInfo& c : page.chars) {
        if (c.space || !c.ocr) {
            continue;
        }
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.bottom + c.top) / 2.0;
        if (cx >= left && cx <= right && cy >= bottom && cy <= top) {
            ++glyphs_on_top;
        }
    }
    if (debug_bg) {
        std::fprintf(
            stderr,
            "BG p%d img L=%.1f B=%.1f R=%.1f T=%.1f page=%.0fx%.0f cover=%.2fx%.2f glyphs=%zu\n",
            page.index + 1, left, bottom, right, top, page.width, page.height,
            on_page_w / page.width, on_page_h / page.height, glyphs_on_top);
    }
    if (on_page_w < kBgMinSpanFraction * page.width ||
        on_page_h < kBgMinSpanFraction * page.height) {
        return false;
    }
    if (on_page_w * on_page_h < kBgMinAreaFraction * page.width * page.height) {
        return false;
    }
    return glyphs_on_top >= kBgMinOcrGlyphs;
}

bool region_holds_prose(const Rect& r, const std::vector<CharInfo>& chars) noexcept {
    const double w = r.right - r.left;
    const double h = r.top - r.bottom;
    const double bx = (std::min)(kProseEdgeInsetRatio * w, kProseMaxEdgeInsetPt);
    const double by = (std::min)(kProseEdgeInsetRatio * h, kProseMaxEdgeInsetPt);
    std::size_t n = 0U;
    for (const CharInfo& c : chars) {
        if (c.space || figure_label_glyph(c)) {
            continue;
        }
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.bottom + c.top) / 2.0;
        if (cx > r.left + bx && cx < r.right - bx && cy > r.bottom + by && cy < r.top - by) {
            if (++n > kMinInteriorProseGlyphs) {
                return true;
            }
        }
    }
    return false;
}

int count_form_vector_paths(FPDF_PAGEOBJECT form, int depth = 0) {
    int n = 0;
    const int count = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT o = FPDFFormObj_GetObject(form, i);
        if (o == nullptr) {
            continue;
        }
        const int type = FPDFPageObj_GetType(o);
        if (type == FPDF_PAGEOBJ_PATH) {
            ++n;
            continue;
        }
        if (type == FPDF_PAGEOBJ_FORM && depth < kMaxFormNestingDepth) {
            n += count_form_vector_paths(o, depth + 1);
        }
    }
    return n;
}

double form_scale(FPDF_PAGEOBJECT form) {
    FS_MATRIX m{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    if (!FPDFPageObj_GetMatrix(form, &m)) {
        return 1.0;
    }
    return 0.5 * (std::fabs(static_cast<double>(m.a)) + std::fabs(static_cast<double>(m.d)));
}

bool is_diagram_form(FPDF_PAGEOBJECT form) {
    return count_form_vector_paths(form) >= kMinDiagramVectorPaths &&
           form_scale(form) <= kMaxDiagramFormScale;
}

BitmapHandle render_region_bitmap(FPDF_PAGE page, double page_w, double page_h, const Rect& rgn) {
    const int w = static_cast<int>(std::lround((rgn.right - rgn.left) * kRegionRenderScale));
    const int h = static_cast<int>(std::lround((rgn.top - rgn.bottom) * kRegionRenderScale));
    if (w < 1 || h < 1 || w > kMaxRenderDimPx || h > kMaxRenderDimPx) {
        return nullptr;
    }

    BitmapHandle bitmap(FPDFBitmap_Create(w, h, 0));
    if (!bitmap) {
        return nullptr;
    }
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, w, h, kOpaqueWhiteArgb);
    const int start_x = static_cast<int>(std::lround(-rgn.left * kRegionRenderScale));
    const int start_y = static_cast<int>(std::lround(-(page_h - rgn.top) * kRegionRenderScale));
    const int size_x = static_cast<int>(std::lround(page_w * kRegionRenderScale));
    const int size_y = static_cast<int>(std::lround(page_h * kRegionRenderScale));
    FPDF_RenderPageBitmap(bitmap.get(), page, start_x, start_y, size_x, size_y, 0, 0);
    return bitmap;
}

constexpr unsigned long kEmptyUtf16PayloadBytes = 2UL;
constexpr int kMaxStructTreeDepth = 32;
constexpr std::size_t kMaxStructTreeNodes = 8192U;
constexpr unsigned char kAsciiControlEnd = 0x20U;
constexpr unsigned char kAsciiDeleteByte = 0x7FU;
constexpr unsigned char kUtf8NelLead = 0xC2U;
constexpr unsigned char kUtf8NelTrail = 0x85U;
constexpr unsigned char kUtf8SepLead = 0xE2U;
constexpr unsigned char kUtf8SepMid = 0x80U;
constexpr unsigned char kUtf8LineSepTrail = 0xA8U;
constexpr unsigned char kUtf8ParaSepTrail = 0xA9U;

std::string sanitize_alt_text(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0U; i < input.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        if (b0 == '\r' || b0 == '\n' || b0 == '\t' || b0 == ' ') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        if (b0 == kUtf8NelLead && i + 1U < input.size() &&
            static_cast<unsigned char>(input[i + 1U]) == kUtf8NelTrail) {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            ++i;
            continue;
        }
        if (b0 == kUtf8SepLead && i + 2U < input.size() &&
            static_cast<unsigned char>(input[i + 1U]) == kUtf8SepMid) {
            const auto b2 = static_cast<unsigned char>(input[i + 2U]);
            if (b2 == kUtf8LineSepTrail || b2 == kUtf8ParaSepTrail) {
                if (!out.empty() && out.back() != ' ') {
                    out.push_back(' ');
                }
                i += 2U;
                continue;
            }
        }
        if (b0 < kAsciiControlEnd || b0 == kAsciiDeleteByte) {
            continue;
        }
        out.push_back(static_cast<char>(b0));
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string read_struct_elem_alt(FPDF_STRUCTELEMENT elem) {
    const unsigned long bytes = FPDF_StructElement_GetAltText(elem, nullptr, 0UL);
    if (bytes <= kEmptyUtf16PayloadBytes) {
        return {};
    }
    std::vector<unsigned char> buf(static_cast<std::size_t>(bytes));
    if (FPDF_StructElement_GetAltText(elem, buf.data(), bytes) == 0UL) {
        return {};
    }
    return sanitize_alt_text(utf16le_to_utf8(buf.data(), buf.size()));
}

void collect_elem_mcid_alt(FPDF_STRUCTELEMENT elem, std::string_view inherited_alt, int depth,
                           std::size_t& visited_nodes,
                           std::unordered_map<int, std::string>& mcid_alt) {
    if (elem == nullptr || depth > kMaxStructTreeDepth || visited_nodes >= kMaxStructTreeNodes) {
        return;
    }
    ++visited_nodes;

    const std::string own_alt = read_struct_elem_alt(elem);
    const std::string_view effective_alt =
        !own_alt.empty() ? std::string_view(own_alt) : inherited_alt;

    const int mcid = FPDF_StructElement_GetMarkedContentID(elem);
    if (mcid >= 0 && !effective_alt.empty()) {
        mcid_alt.try_emplace(mcid, effective_alt);
    }

    const int child_count = FPDF_StructElement_CountChildren(elem);
    for (int ci = 0; ci < child_count && visited_nodes < kMaxStructTreeNodes; ++ci) {
        collect_elem_mcid_alt(FPDF_StructElement_GetChildAtIndex(elem, ci), effective_alt,
                              depth + 1, visited_nodes, mcid_alt);
    }
}

std::unordered_map<int, std::string> load_page_mcid_alt_map(FPDF_PAGE page) {
    StructTreeHandle tree(FPDF_StructTree_GetForPage(page));
    if (!tree) {
        return {};
    }
    std::unordered_map<int, std::string> mcid_alt;
    std::size_t visited_nodes = 0U;
    const int root_count = FPDF_StructTree_CountChildren(tree.get());
    for (int i = 0; i < root_count && visited_nodes < kMaxStructTreeNodes; ++i) {
        collect_elem_mcid_alt(FPDF_StructTree_GetChildAtIndex(tree.get(), i), {}, 1, visited_nodes,
                              mcid_alt);
    }
    return mcid_alt;
}

}  // namespace

void collect_page_raster_images(FPDF_DOCUMENT doc, FPDF_PAGE page, const PageData& page_data,
                                const ExtractOptions& options, std::size_t page_vec_index,
                                std::unordered_set<std::uint64_t>& captured_hashes,
                                std::vector<PendingPageImage>& pending,
                                std::vector<Rect>& suppressed_scans) {
    bool loaded_struct_tree = false;
    std::unordered_map<int, std::string> mcid_alt;

    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
            continue;
        }

        float raw_left = 0.0F;
        float raw_bottom = 0.0F;
        float raw_right = 0.0F;
        float raw_top = 0.0F;
        if (!FPDFPageObj_GetBounds(obj, &raw_left, &raw_bottom, &raw_right, &raw_top) ||
            !std::isfinite(raw_left) || !std::isfinite(raw_bottom) || !std::isfinite(raw_right) ||
            !std::isfinite(raw_top)) {
            continue;
        }
        const float left = (std::min)(raw_left, raw_right);
        const float right = (std::max)(raw_left, raw_right);
        const float bottom = (std::min)(raw_bottom, raw_top);
        const float top = (std::max)(raw_bottom, raw_top);
        if (right - left < kMinObjectExtentPt || top - bottom < kMinObjectExtentPt) {
            continue;
        }
        if (is_page_background_image(left, bottom, right, top, page_data)) {
            suppressed_scans.push_back(Rect{left, bottom, right, top});
            continue;
        }
        if (options.image_dir.empty()) {
            continue;
        }

        BitmapHandle bitmap(FPDFImageObj_GetRenderedBitmap(doc, page, obj));
        if (!bitmap) {
            continue;
        }

        const BitmapView view = make_bitmap_view(bitmap.get());
        const std::uint64_t hash = hash_bitmap(view);
        if (hash == 0U) {
            continue;
        }

        PendingPageImage item;
        item.kind = PendingImageKind::RasterDedup;
        item.page_vec_index = page_vec_index;
        item.bounds = Rect{left, bottom, right, top};
        item.hash = hash;

        FPDF_IMAGEOBJ_METADATA meta{};
        if (FPDFImageObj_GetImageMetadata(obj, page, &meta) && meta.marked_content_id >= 0) {
            if (!loaded_struct_tree) {
                mcid_alt = load_page_mcid_alt_map(page);
                loaded_struct_tree = true;
            }
            const auto alt_it = mcid_alt.find(meta.marked_content_id);
            if (alt_it != mcid_alt.end()) {
                item.alt = alt_it->second;
            }
        }

        if (captured_hashes.insert(hash).second) {
            item.raster = copy_bitmap_raster(bitmap.get());
        }
        pending.push_back(std::move(item));
    }
}

void collect_scan_diagram_bands(FPDF_PAGE page, const PageData& page_data,
                                std::size_t page_vec_index, const Rect& scan,
                                std::vector<PendingPageImage>& pending) {
    std::vector<Interval> ivals;
    ivals.reserve(page_data.chars.size());
    for (const CharInfo& c : page_data.chars) {
        if (c.space) {
            continue;
        }
        if ((c.left + c.right) / 2.0 > kOuterMarginStampRatio * page_data.width) {
            continue;  // margin stamps
        }
        ivals.push_back({c.bottom, c.top});
    }
    if (ivals.size() < 2U) {
        return;
    }
    std::sort(ivals.begin(), ivals.end(),
              [](const Interval& a, const Interval& b) noexcept { return a.lo < b.lo; });

    std::vector<Interval> gaps;
    double cur_hi = ivals[0].hi;
    for (std::size_t i = 1U; i < ivals.size(); ++i) {
        if (ivals[i].lo > cur_hi + kMinDiagramBandHeight) {
            gaps.push_back({cur_hi, ivals[i].lo});
        }
        cur_hi = (std::max)(cur_hi, ivals[i].hi);
    }

    for (const Interval& gap : gaps) {
        const Rect band{(std::min)(scan.left, 0.0),
                        (std::max)(gap.lo + kBandGapInsetPt, scan.bottom),
                        (std::max)(scan.right, page_data.width),
                        (std::min)(gap.hi - kBandGapInsetPt, scan.top)};
        if (band.top - band.bottom < kMinTrimmedBandRatio * kMinDiagramBandHeight) {
            continue;
        }
        BitmapHandle bitmap = render_region_bitmap(page, page_data.width, page_data.height, band);
        if (!bitmap) {
            continue;
        }

        // Blank out any margin stamp level with the band (an on-page DRM mark).
        for (const CharInfo& c : page_data.chars) {
            if (c.space || (c.left + c.right) / 2.0 <= kOuterMarginStampRatio * page_data.width) {
                continue;
            }
            if (c.top < band.bottom || c.bottom > band.top) {
                continue;
            }
            const int px = (std::max)(
                0, static_cast<int>(std::lround((c.left - kBandGapInsetPt - band.left) *
                                                kRegionRenderScale)));
            const int py = (std::max)(
                0, static_cast<int>(std::lround((band.top - c.top - kBandGapInsetPt) *
                                                kRegionRenderScale)));
            const int pw = static_cast<int>(
                std::lround((c.right - c.left + 2.0 * kBandGapInsetPt) * kRegionRenderScale));
            const int ph = static_cast<int>(
                std::lround((c.top - c.bottom + 2.0 * kBandGapInsetPt) * kRegionRenderScale));
            FPDFBitmap_FillRect(bitmap.get(), px, py, pw, ph, kOpaqueWhiteArgb);
        }

        RasterImage raster = copy_bitmap_raster(bitmap.get());
        if (raster.empty()) {
            continue;
        }
        PendingPageImage item;
        item.kind = PendingImageKind::ScanBand;
        item.page_vec_index = page_vec_index;
        item.bounds = band;
        item.raster = std::move(raster);
        pending.push_back(std::move(item));
    }
}

std::vector<std::pair<double, double>> diagram_prose_bands(const Rect& r,
                                                           const std::vector<CharInfo>& chars) {
    const double region_w = r.right - r.left;
    std::vector<const CharInfo*> inside;
    inside.reserve(chars.size());
    for (const CharInfo& c : chars) {
        if (c.space || c.size <= 0.0) {
            continue;
        }
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.bottom + c.top) / 2.0;
        if (cx > r.left && cx < r.right && cy > r.bottom && cy < r.top) {
            inside.push_back(&c);
        }
    }
    std::sort(inside.begin(), inside.end(),
              [](const CharInfo* a, const CharInfo* b) noexcept { return a->y > b->y; });

    std::vector<std::pair<double, double>> bands;
    std::size_t i = 0U;
    while (i < inside.size()) {
        std::size_t j = i;
        const double y0 = inside[i]->y;
        const double sz = inside[i]->size;
        double line_l = inside[i]->left;
        double line_r = inside[i]->right;
        double line_b = inside[i]->bottom;
        double line_t = inside[i]->top;
        double covered = 0.0;
        int nchars = 0;
        while (j < inside.size() && std::fabs(inside[j]->y - y0) <= kBaselineClusterRatio * sz) {
            const CharInfo* c = inside[j];
            line_l = (std::min)(line_l, c->left);
            line_r = (std::max)(line_r, c->right);
            line_b = (std::min)(line_b, c->bottom);
            line_t = (std::max)(line_t, c->top);
            covered += (std::max)(0.0, c->right - c->left);
            if (!figure_label_glyph(*c)) {
                ++nchars;
            }
            ++j;
        }
        const double span = line_r - line_l;
        const double width_frac = region_w > 0.0 ? span / region_w : 0.0;
        const double fill = span > 0.0 ? covered / span : 0.0;

        // Sort this baseline slice in place by x to measure the widest inter-glyph gap.
        std::sort(inside.begin() + static_cast<std::ptrdiff_t>(i),
                  inside.begin() + static_cast<std::ptrdiff_t>(j),
                  [](const CharInfo* a, const CharInfo* b) noexcept { return a->left < b->left; });
        double max_gap = 0.0;
        for (std::size_t g = i + 1U; g < j; ++g) {
            max_gap = (std::max)(max_gap, inside[g]->left - inside[g - 1U]->right);
        }

        const bool prose = nchars >= kMinProseLineChars && width_frac >= kMinProseWidthFraction &&
                           fill >= kMinProseFillFraction && max_gap <= kMaxProseGapRatio * sz;
        if (prose) {
            bands.emplace_back(line_b, line_t);
        }
        i = j;
    }
    return bands;
}

std::vector<FigureRegion> detect_figure_regions(FPDF_PAGE page, double page_w, double page_h,
                                                const std::vector<CharInfo>& chars) {
    struct Seed {
        Rect r;
        bool diagram = false;
    };
    std::vector<Seed> seeds;
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj == nullptr) {
            continue;
        }
        const int type = FPDFPageObj_GetType(obj);
        if (type != FPDF_PAGEOBJ_FORM && type != FPDF_PAGEOBJ_SHADING) {
            continue;
        }
        float l = 0.0F;
        float b = 0.0F;
        float r = 0.0F;
        float t = 0.0F;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) {
            continue;
        }
        if (r - l < kMinObjectExtentPt || t - b < kMinObjectExtentPt) {
            continue;
        }
        seeds.push_back({{l, b, r, t}, type == FPDF_PAGEOBJ_FORM && is_diagram_form(obj)});
    }

    std::vector<FigureRegion> regions;
    std::vector<char> used(seeds.size(), 0);
    for (std::size_t i = 0U; i < seeds.size(); ++i) {
        if (used[i] != 0) {
            continue;
        }
        Rect cur = seeds[i].r;
        bool diagram = seeds[i].diagram;
        used[i] = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t j = 0U; j < seeds.size(); ++j) {
                if (used[j] != 0 || !rects_close(cur, seeds[j].r, kFigureClusterGapPt)) {
                    continue;
                }
                cur.left = (std::min)(cur.left, seeds[j].r.left);
                cur.bottom = (std::min)(cur.bottom, seeds[j].r.bottom);
                cur.right = (std::max)(cur.right, seeds[j].r.right);
                cur.top = (std::max)(cur.top, seeds[j].r.top);
                diagram = diagram || seeds[j].diagram;
                used[j] = 1;
                grew = true;
            }
        }
        regions.push_back({cur, diagram});
    }

    std::vector<FigureRegion> kept;
    kept.reserve(regions.size());
    for (const FigureRegion& rg : regions) {
        double w = rg.rect.right - rg.rect.left;
        double h = rg.rect.top - rg.rect.bottom;
        if (w > kMaxFigureOvershootRatio * page_w || h > kMaxFigureOvershootRatio * page_h) {
            continue;
        }
        const Rect box = {(std::max)(rg.rect.left, 0.0), (std::max)(rg.rect.bottom, 0.0),
                          (std::min)(rg.rect.right, page_w), (std::min)(rg.rect.top, page_h)};
        w = box.right - box.left;
        h = box.top - box.bottom;
        if (w < kMinFigureWidthRatio * page_w || h < kMinFigureHeightRatio * page_h) {
            continue;
        }
        if (!rg.diagram && region_holds_prose(box, chars)) {
            continue;
        }
        kept.push_back({box, rg.diagram});
    }
    return kept;
}

void erase_figure_text_labels(std::span<const FigureRegion> figures,
                              std::vector<CharInfo>& chars) {
    if (figures.empty()) {
        return;
    }
    std::vector<std::vector<std::pair<double, double>>> keep_bands(figures.size());
    for (std::size_t k = 0U; k < figures.size(); ++k) {
        if (figures[k].diagram) {
            keep_bands[k] = diagram_prose_bands(figures[k].rect, chars);
        }
    }

    std::erase_if(chars, [&](const CharInfo& c) noexcept {
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.bottom + c.top) / 2.0;
        for (std::size_t k = 0U; k < figures.size(); ++k) {
            const FigureRegion& f = figures[k];
            if (cx < f.rect.left || cx > f.rect.right || cy < f.rect.bottom || cy > f.rect.top) {
                continue;
            }
            if (f.diagram) {
                const bool prose =
                    std::any_of(keep_bands[k].begin(), keep_bands[k].end(),
                                [cy](const std::pair<double, double>& b) noexcept {
                                    return cy >= b.first && cy <= b.second;
                                });
                if (!prose) {
                    return true;
                }
            } else if (figure_label_glyph(c)) {
                return true;
            }
        }
        return false;
    });
}

Rect clip_render_box(const Rect& rgn, const std::vector<CharInfo>& chars) noexcept {
    const double h = rgn.top - rgn.bottom;
    if (h <= 0.0) {
        return rgn;
    }
    const double band = (std::min)(kClipEdgeBandRatio * h, kClipMaxEdgeBandPt);
    double new_top = rgn.top;
    double new_bottom = rgn.bottom;
    for (const CharInfo& c : chars) {
        if (c.space || figure_label_glyph(c)) {
            continue;
        }
        const double cx = (c.left + c.right) / 2.0;
        const double cy = (c.bottom + c.top) / 2.0;
        if (cx < rgn.left || cx > rgn.right || cy <= rgn.bottom || cy >= rgn.top) {
            continue;
        }
        if (cy > rgn.top - band) {
            new_top = (std::min)(new_top, c.bottom - kClipGlyphClearancePt);
        } else if (cy < rgn.bottom + band) {
            new_bottom = (std::max)(new_bottom, c.top + kClipGlyphClearancePt);
        }
    }
    if (new_top <= new_bottom + kMinClippedHeightPt) {
        return rgn;
    }
    return {rgn.left, new_bottom, rgn.right, new_top};
}

void collect_vector_figure_renders(FPDF_PAGE page, const PageData& page_data,
                                   std::span<const FigureRegion> figures,
                                   std::size_t page_vec_index,
                                   std::vector<PendingPageImage>& pending) {
    for (const FigureRegion& f : figures) {
        const Rect box = clip_render_box(f.rect, page_data.chars);
        const BitmapHandle bitmap =
            render_region_bitmap(page, page_data.width, page_data.height, box);
        if (!bitmap) {
            continue;
        }
        RasterImage raster = copy_bitmap_raster(bitmap.get());
        if (raster.empty()) {
            continue;
        }
        PendingPageImage item;
        item.kind = PendingImageKind::VectorRegion;
        item.page_vec_index = page_vec_index;
        item.bounds = box;
        item.raster = std::move(raster);
        pending.push_back(std::move(item));
    }
}

void flush_pending_images(std::vector<PendingPageImage>& pending, std::vector<PageData>& pages,
                          const ExtractOptions& options) {
    int image_counter = 0;
    std::unordered_map<std::uint64_t, std::string> image_paths;

    for (PendingPageImage& item : pending) {
        if (item.page_vec_index >= pages.size()) {
            continue;
        }
        PageData& page_data = pages[item.page_vec_index];

        switch (item.kind) {
            case PendingImageKind::RasterDedup: {
                std::string ref;
                const auto it = image_paths.find(item.hash);
                if (it != image_paths.end()) {
                    ref = it->second;
                } else if (!options.image_dir.empty() && !item.raster.empty()) {
                    const std::string file_name =
                        "image_" + std::to_string(image_counter + 1) + ".png";
                    const std::filesystem::path out = options.image_dir / file_name;
                    if (write_bitmap_png(item.raster.view(), out)) {
                        ++image_counter;
                        ref = options.image_ref_prefix + file_name;
                        image_paths.emplace(item.hash, ref);
                    }
                }
                if (!ref.empty()) {
                    page_data.images.push_back(ImageRef{std::move(ref), item.bounds.left,
                                                        item.bounds.top, item.bounds.bottom,
                                                        item.bounds.right, std::move(item.alt)});
                }
                break;
            }
            case PendingImageKind::ScanBand: {
                const std::string file_name =
                    "image_" + std::to_string(image_counter + 1) + ".png";
                const std::filesystem::path out = options.image_dir / file_name;
                if (!write_scan_band_png(item.raster.view(), out, kScanBandPadPx,
                                         kMinBandInkFraction)) {
                    continue;
                }
                ++image_counter;
                page_data.images.push_back(
                    ImageRef{options.image_ref_prefix + file_name, item.bounds.left,
                             item.bounds.top, item.bounds.bottom, item.bounds.right,
                             std::move(item.alt)});
                break;
            }
            case PendingImageKind::VectorRegion: {
                const std::string file_name =
                    "image_" + std::to_string(image_counter + 1) + ".png";
                const std::filesystem::path out = options.image_dir / file_name;
                if (write_bitmap_png(item.raster.view(), out)) {
                    ++image_counter;
                    page_data.images.push_back(
                        ImageRef{options.image_ref_prefix + file_name, item.bounds.left,
                                 item.bounds.top, item.bounds.bottom, item.bounds.right,
                                 std::move(item.alt)});
                }
                break;
            }
        }
    }
}

}  // namespace pdf2md::pdf
