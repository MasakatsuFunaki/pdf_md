#pragma once

#include "pdf2md/document_model.h"
#include "pdf2md/image_extractor.h"
#include "pdf2md/model/geometry.h"
#include "pdf2md/options.h"

#include <fpdfview.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdf2md::pdf {

// A detected vector figure region. `diagram` marks a pure-vector illustration
// (UML, flowchart, architecture box diagram) whose interior text is scattered
// label runs rather than prose.
struct FigureRegion {
    model::Rect rect;
    bool diagram = false;
};

enum class PendingImageKind : std::uint8_t {
    RasterDedup,
    ScanBand,
    VectorRegion,
};

// In-memory raster task captured while holding PdfiumInit::mutex() and flushed
// (PNG compression + disk write) outside the lock.
struct PendingPageImage {
    PendingImageKind kind = PendingImageKind::RasterDedup;
    std::size_t page_vec_index = 0;
    model::Rect bounds;
    std::uint64_t hash = 0;
    std::string alt;
    RasterImage raster;
};

constexpr double kMinOrdinaryGlyphSizePt = 4.0;
constexpr double kDegenerateBoxRatio = 2.0;

constexpr bool figure_label_glyph(const CharInfo& c) noexcept {
    if (c.size <= 0.0) {
        return false;
    }
    if (c.size < kMinOrdinaryGlyphSizePt) {
        return true;
    }
    return (c.top - c.bottom) > kDegenerateBoxRatio * c.size;
}

void collect_page_raster_images(FPDF_DOCUMENT doc, FPDF_PAGE page, const PageData& page_data,
                                const ExtractOptions& options, std::size_t page_vec_index,
                                std::unordered_set<std::uint64_t>& captured_hashes,
                                std::vector<PendingPageImage>& pending,
                                std::vector<model::Rect>& suppressed_scans);

void collect_scan_diagram_bands(FPDF_PAGE page, const PageData& page_data,
                                std::size_t page_vec_index, const model::Rect& scan,
                                std::vector<PendingPageImage>& pending);

std::vector<FigureRegion> detect_figure_regions(FPDF_PAGE page, double page_w, double page_h,
                                                const std::vector<CharInfo>& chars);

std::vector<std::pair<double, double>> diagram_prose_bands(const model::Rect& r,
                                                           const std::vector<CharInfo>& chars);

void erase_figure_text_labels(std::span<const FigureRegion> figures,
                              std::vector<CharInfo>& chars);

model::Rect clip_render_box(const model::Rect& rgn, const std::vector<CharInfo>& chars) noexcept;

void collect_vector_figure_renders(FPDF_PAGE page, const PageData& page_data,
                                   std::span<const FigureRegion> figures,
                                   std::size_t page_vec_index,
                                   std::vector<PendingPageImage>& pending);

// Executes PNG deflate encoding and filesystem writes outside PdfiumInit::mutex(),
// preserving exact sequential image numbering and content-hash deduplication.
void flush_pending_images(std::vector<PendingPageImage>& pending, std::vector<PageData>& pages,
                          const ExtractOptions& options);

}  // namespace pdf2md::pdf
