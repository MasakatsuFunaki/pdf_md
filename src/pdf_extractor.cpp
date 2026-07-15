#include "pdf_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include "image_extractor.h"
#include "utf8.h"

// Adapter: this translation unit is the sole adapter between PDFium's C API
// (raw opaque handles, UTF-16LE text buffers, device/page matrices) and the
// domain model in document_model.h (CharInfo / ImageRef / RuleSegment /
// PageData). No PDFium type leaks past this file's boundary.
namespace pdf2md {

namespace {

struct PdfiumLibrary {
    PdfiumLibrary() { FPDF_InitLibrary(); }
    ~PdfiumLibrary() { FPDF_DestroyLibrary(); }
};

// The process-wide PDFium session: a Meyers Singleton (function-local static) --
// lazy, thread-safe init with ordered teardown at process exit. Deliberately the
// only singleton in the codebase. PDFium is not thread-safe, so a single
// process-wide init is the right granularity for this tool.
void ensure_library() {
    static PdfiumLibrary lib;
}

// RAII owners for the PDFium C handles used in this translation unit. Each is a
// move-only unique_ptr over the opaque handle, releasing through the matching
// FPDF_* teardown call; at scope exit destruction runs in reverse construction
// order, preserving PDFium's requirement that a text page and any owned bitmap
// be released before the page, and the page before the document. The stored
// pointer is the raw handle, so .get() yields the exact type PDFium expects.
struct FpdfDocumentDeleter {
    void operator()(FPDF_DOCUMENT h) const { FPDF_CloseDocument(h); }
};
struct FpdfPageDeleter {
    void operator()(FPDF_PAGE h) const { FPDF_ClosePage(h); }
};
struct FpdfTextPageDeleter {
    void operator()(FPDF_TEXTPAGE h) const { FPDFText_ClosePage(h); }
};
struct FpdfBitmapDeleter {
    void operator()(FPDF_BITMAP h) const { FPDFBitmap_Destroy(h); }
};
using DocumentHandle = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, FpdfDocumentDeleter>;
using PageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, FpdfPageDeleter>;
using TextPageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, FpdfTextPageDeleter>;
using BitmapHandle = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, FpdfBitmapDeleter>;

std::vector<char> read_file(const std::filesystem::path& file) {
    std::ifstream is(file, std::ios::binary);
    if (!is) throw PdfError("cannot open file: " + file.string());
    is.seekg(0, std::ios::end);
    std::streamoff len = is.tellg();
    if (len < 0) throw PdfError("cannot read file: " + file.string());
    is.seekg(0, std::ios::beg);
    std::vector<char> data(static_cast<size_t>(len));
    if (len > 0) is.read(data.data(), len);
    if (!is) throw PdfError("cannot read file: " + file.string());
    return data;
}

std::string load_error_message(unsigned long err) {
    switch (err) {
        case FPDF_ERR_FILE: return "file not found or could not be read";
        case FPDF_ERR_FORMAT: return "file is not a valid PDF";
        case FPDF_ERR_PASSWORD: return "password required or incorrect password (use --password)";
        case FPDF_ERR_SECURITY: return "unsupported security scheme";
        case FPDF_ERR_PAGE: return "page not found or content error";
        default: return "unknown error loading document";
    }
}

std::string get_meta_text(FPDF_DOCUMENT doc, const char* tag) {
    unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned char> buf(bytes);
    FPDF_GetMetaText(doc, tag, buf.data(), bytes);
    return utf16le_to_utf8(buf.data(), buf.size());
}

bool name_contains(const std::string& haystack, const char* needle) {
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + std::strlen(needle),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

// Common ligatures and private-use bullets normalized to plain code points.
const char32_t* expand_special(char32_t cp) {
    switch (cp) {
        case 0xFB00: return U"ff";
        case 0xFB01: return U"fi";
        case 0xFB02: return U"fl";
        case 0xFB03: return U"ffi";
        case 0xFB04: return U"ffl";
        case 0xFB05: return U"ft";
        case 0xFB06: return U"st";
        default: return nullptr;
    }
}

char32_t map_private_use(char32_t cp) {
    switch (cp) {
        case 0xF0B7: return 0x2022;  // Symbol/Wingdings bullet
        case 0xF0A7: return 0x25AA;  // Wingdings small square
        case 0xF0D8: return 0x2023;  // Wingdings arrow bullet
        case 0xF0FC: return 0x2713;  // Wingdings check mark
        default: return cp;
    }
}

// TeX's Computer Modern math-extension font (CMEX) carries the large operators
// (summation, product, ...). Those glyphs have no ToUnicode entry, so PDFium
// decodes them as the Latin letter occupying the same font slot -- a summation
// comes through as 'P'. Map the operator slots back to their real code points so
// display/inline math reconstruction sees a real \sum rather than a stray "P".
// (cmex encoding: 0x50/'P' and 0x58/'X' = summation, 0x51/'Q' and 0x59/'Y' =
// product.) This is a property of the standard font, not of any one document.
char32_t map_math_extension_glyph(char32_t cp, const std::string& font_name) {
    if (!name_contains(font_name, "cmex")) return cp;
    switch (cp) {
        case U'P': case U'X': return 0x2211;  // n-ary summation
        case U'Q': case U'Y': return 0x220F;  // n-ary product
        default: return cp;
    }
}

struct FontProps {
    bool bold = false;
    bool italic = false;
    bool mono = false;
};

// PDF font descriptor flag bits (PDF 32000-1:2008, table 123).
constexpr int kFontFixedPitch = 1 << 0;
constexpr int kFontItalic = 1 << 6;
constexpr int kFontForceBold = 1 << 18;

FontProps font_props(FPDF_TEXTPAGE tp, int index, const std::string& name, int flags) {
    FontProps props;
    int weight = FPDFText_GetFontWeight(tp, index);
    // TeX's Computer Modern bold families ("CMBX", "CMBSY", "CMMIB", "CMBXTI")
    // declare a weight around 570 rather than 700 and carry no "bold" in the
    // name, so bold math (e.g. \mathbf vectors) would otherwise read as regular.
    props.bold = (weight >= 600) || (flags & kFontForceBold) || name_contains(name, "bold") ||
                 name_contains(name, "black") || name_contains(name, "heavy") ||
                 name_contains(name, "cmbx") || name_contains(name, "cmbsy") ||
                 name_contains(name, "cmmib");
    props.italic = (flags & kFontItalic) || name_contains(name, "italic") ||
                   name_contains(name, "oblique");
    props.mono = (flags & kFontFixedPitch) || name_contains(name, "courier") ||
                 name_contains(name, "mono") || name_contains(name, "consol");
    return props;
}

// An axis-aligned page-space rectangle (also used by the vector-figure code
// further below).
struct FigureRect {
    double left = 0, bottom = 0, right = 0, top = 0;
};

// A raster image spanning nearly the whole page underneath an OCR text layer is
// the page's background -- the scan bitmap of a scanned+OCR'd book page -- not
// a figure. Inlining it would duplicate every page of text as a picture, so
// such images are skipped. Only glyphs from a repaired OCR layer (CharInfo.ocr,
// set by repair_ocr_text_layer before images are extracted) count as "text over
// the scan": a designed page whose full-bleed photo carries a real typeset
// title (a slide, a poster, a cover) keeps its image -- there the bitmap IS the
// content, and so does a scan page with no recognized text.
bool is_page_background_image(float left, float bottom, float right, float top,
                              const PageData& page) {
    static const bool debug_bg = std::getenv("PDF2MD_DEBUG_BG") != nullptr;
    const double on_page_w = std::min<double>(right, page.width) - std::max<double>(left, 0.0);
    const double on_page_h = std::min<double>(top, page.height) - std::max<double>(bottom, 0.0);
    size_t glyphs_on_top = 0;
    for (const CharInfo& c : page.chars) {
        if (c.space || !c.ocr) continue;
        double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
        if (cx >= left && cx <= right && cy >= bottom && cy <= top) ++glyphs_on_top;
    }
    if (debug_bg)
        std::fprintf(stderr,
                     "BG p%d img L=%.1f B=%.1f R=%.1f T=%.1f page=%.0fx%.0f cover=%.2fx%.2f glyphs=%zu\n",
                     page.index + 1, left, bottom, right, top, page.width, page.height,
                     on_page_w / page.width, on_page_h / page.height, glyphs_on_top);
    // Geometry: most of the page in both dimensions (a scan can sit inside the
    // page margins, so the bar is deliberately loose); the decisive signal is
    // the body of real text drawn over the image.
    if (on_page_w < 0.60 * page.width || on_page_h < 0.60 * page.height) return false;
    if (on_page_w * on_page_h < 0.55 * page.width * page.height) return false;
    return glyphs_on_top >= 25;
}

void extract_page_images(FPDF_DOCUMENT doc, FPDF_PAGE page, PageData& page_data,
                         const ExtractOptions& options, int& image_counter,
                         std::unordered_map<uint64_t, std::string>& image_paths,
                         std::vector<FigureRect>& suppressed_scans) {
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;

        float left = 0, bottom = 0, right = 0, top = 0;
        if (!FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) continue;
        // Skip degenerate/decorative slivers.
        if (right - left < 4 || top - bottom < 4) continue;
        // Skip the page-background scan/backdrop under a text layer; remember
        // its bounds so the scan's text-free bands can be mined for diagrams.
        if (is_page_background_image(left, bottom, right, top, page_data)) {
            suppressed_scans.push_back({left, bottom, right, top});
            continue;
        }

        // FPDFImageObj_GetRenderedBitmap returns a freshly rendered bitmap the
        // caller owns; the raw handle is passed by value to hash_bitmap /
        // write_bitmap_png (which do not take ownership) and destroyed here at
        // scope exit.
        BitmapHandle bitmap(FPDFImageObj_GetRenderedBitmap(doc, page, obj));
        if (!bitmap) continue;

        uint64_t hash = hash_bitmap(bitmap.get());
        std::string ref;
        auto it = image_paths.find(hash);
        if (it != image_paths.end()) {
            ref = it->second;
        } else {
            ++image_counter;
            std::string file_name = "image_" + std::to_string(image_counter) + ".png";
            std::filesystem::path out = options.image_dir / file_name;
            if (write_bitmap_png(bitmap.get(), out)) {
                ref = options.image_ref_prefix + file_name;
                image_paths.emplace(hash, ref);
            }
        }
        if (!ref.empty()) {
            page_data.images.push_back(ImageRef{ref, left, top, bottom});
        }
    }
}

// ---------------------------------------------------------------- rules
//
// Table borders are drawn as vector paths: either thin stroked line segments or
// thin filled rectangles. Collecting the axis-aligned edges of every PATH object
// gives the ruling lattice a bordered/gridded table stands on, which the layout
// stage uses to recover grids whose cells wrap too heavily for text geometry
// alone to detect. Only near-horizontal and near-vertical edges longer than a
// few points are kept; a page's diagram artwork also contributes edges, but the
// layout stage requires a genuine multi-cell lattice (and cell text) before it
// treats any of them as a table.

// Applies a page-object matrix to a point (path points are in the object's own
// space; the matrix places them on the page).
void apply_matrix(const FS_MATRIX& m, double x, double y, double& ox, double& oy) {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

void extract_page_rules(FPDF_PAGE page, PageData& page_data) {
    constexpr double kEps = 1.6;      // max off-axis deviation for a rule
    constexpr double kMinLen = 6.0;   // shortest edge kept
    const double page_w = page_data.width, page_h = page_data.height;
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) continue;

        FS_MATRIX m{1, 0, 0, 1, 0, 0};
        if (!FPDFPageObj_GetMatrix(obj, &m)) m = {1, 0, 0, 1, 0, 0};

        const int nseg = FPDFPath_CountSegments(obj);
        if (nseg < 2) continue;
        double px = 0, py = 0, sx = 0, sy = 0;  // previous point, subpath start
        bool have_prev = false;
        for (int s = 0; s < nseg; ++s) {
            FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, s);
            if (!seg) continue;
            float fx = 0, fy = 0;
            if (!FPDFPathSegment_GetPoint(seg, &fx, &fy)) continue;
            double x = 0, y = 0;
            apply_matrix(m, fx, fy, x, y);
            int type = FPDFPathSegment_GetType(seg);

            auto emit_edge = [&](double ax, double ay, double bx, double by) {
                double dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
                if (dy <= kEps && dx >= kMinLen) {
                    double yy = (ay + by) / 2.0;
                    page_data.rules.push_back(
                        {std::min(ax, bx), yy, std::max(ax, bx), yy, true});
                } else if (dx <= kEps && dy >= kMinLen) {
                    double xx = (ax + bx) / 2.0;
                    page_data.rules.push_back(
                        {xx, std::min(ay, by), xx, std::max(ay, by), false});
                }
            };

            if (type == FPDF_SEGMENT_MOVETO) {
                px = sx = x;
                py = sy = y;
                have_prev = true;
            } else if (type == FPDF_SEGMENT_LINETO) {
                if (have_prev) emit_edge(px, py, x, y);
                px = x;
                py = y;
                have_prev = true;
            } else {  // BEZIERTO or unknown: skip curve, resume from its endpoint
                px = x;
                py = y;
                have_prev = true;
            }
            // A closed subpath's implicit edge back to its start closes a
            // filled/stroked rectangle border.
            if (FPDFPathSegment_GetClose(seg) && have_prev) emit_edge(x, y, sx, sy);
        }
    }
    // Drop rules wholly outside the page (clipped/degenerate artwork).
    std::erase_if(page_data.rules, [&](const RuleSegment& r) {
        return r.x1 < -2 || r.y1 < -2 || r.x0 > page_w + 2 || r.y0 > page_h + 2;
    });
}

// ---------------------------------------------------------------- figures
//
// Scientific figures are frequently drawn with vector graphics wrapped in a
// form XObject (or a shading), not embedded as a raster image, so the raster
// path above never sees them. They also carry their own baked-in labels: axis
// ticks and annotations set with a degenerate text matrix (a nominal font size
// near 1.0 scaled up by the matrix, so the glyph boxes dwarf the reported size)
// and, often, invisible overlay text. Those labels are not real document prose:
// left in the text stream they interleave with a nearby heading or leak as
// letter-spaced noise. The two routines below (1) locate figure regions from the
// bounds of the vector objects and (2) mark such degenerate label glyphs, so the
// caller can rasterize each region and drop its internal labels while keeping the
// real caption/heading that merely grazes the region edge.

// A detected figure region. `diagram` marks a pure-vector illustration (UML,
// flowchart, architecture box diagram) whose interior text is scattered label
// runs, not prose: every interior glyph is dropped, not only degenerate ones.
struct FigureRegion {
    FigureRect rect;
    bool diagram = false;
};

// True when two rectangles overlap or lie within `gap` of each other on both
// axes (used to fuse the parts of a multi-panel figure into one region).
bool rects_close(const FigureRect& a, const FigureRect& b, double gap) {
    return a.left - gap <= b.right && b.left - gap <= a.right &&
           a.bottom - gap <= b.top && b.bottom - gap <= a.top;
}

// A glyph that is part of a figure's baked-in labelling rather than document
// prose. Such labels are set with a degenerate text matrix: a sub-point nominal
// font size scaled up by the matrix, so the drawn box dwarfs the reported size
// (ordinary text has a box height below its point size). The sub-point size also
// catches the label's zero-height word-break spaces, which carry no box to
// measure. Real captions/headings keep ordinary point sizes and are not matched.
bool figure_label_glyph(const CharInfo& c) {
    if (c.size <= 0) return false;
    if (c.size < 4.0) return true;
    return (c.top - c.bottom) > 2.0 * c.size;
}

// True when a region holds substantial ordinary horizontal text in its interior:
// the signature of a page whose *content* is wrapped in a form XObject (or sits
// over a full-page shading), not a figure. A genuine illustration's interior text
// is rotated (already dropped) or degenerate figure labelling; only a caption or
// heading grazing the region edge is ordinary, and the edge band excludes it.
bool region_holds_prose(const FigureRect& r, const std::vector<CharInfo>& chars) {
    const double w = r.right - r.left, h = r.top - r.bottom;
    const double bx = std::min(0.15 * w, 40.0), by = std::min(0.15 * h, 40.0);
    size_t n = 0;
    for (const CharInfo& c : chars) {
        if (c.space || figure_label_glyph(c)) continue;
        double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
        if (cx > r.left + bx && cx < r.right - bx && cy > r.bottom + by && cy < r.top - by)
            if (++n > 25) return true;
    }
    return false;
}

// A diagram's bounding box can overhang the artwork and swallow a caption or a
// body sentence that sits just above/below it. Those lines are ordinary running
// text, not the diagram's own scattered interior labels, so they must survive: a
// dropped caption is silent data loss. This returns the vertical bands of the
// region's interior text lines that read as prose -- long, densely filled and
// spanning much of the region width -- so the caller can keep their glyphs while
// still dropping the short, isolated label lines. Same isolation/fill signal as
// region_holds_prose, applied per baseline line.
//
// The widest single inter-glyph gap on a line is the deciding signal between the
// two. A caption or wrapped body sentence is a single run whose largest gap is a
// word space -- a fraction of the font size even under full justification. A row
// of diagram box labels reaches across the region too and can pack each label
// densely enough to clear the fill bar, but its clusters are separated by the
// wide gutters between boxes (several times the font size). A line is only kept as
// prose when its largest gap stays word-sized, so multi-cluster label rows fail.
constexpr double kMaxProseGapRatio = 1.5;
std::vector<std::pair<double, double>> diagram_prose_bands(
        const FigureRect& r, const std::vector<CharInfo>& chars) {
    const double region_w = r.right - r.left;
    std::vector<const CharInfo*> inside;
    for (const CharInfo& c : chars) {
        if (c.space || c.size <= 0) continue;
        double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
        if (cx > r.left && cx < r.right && cy > r.bottom && cy < r.top)
            inside.push_back(&c);
    }
    std::sort(inside.begin(), inside.end(),
              [](const CharInfo* a, const CharInfo* b) { return a->y > b->y; });

    std::vector<std::pair<double, double>> bands;
    size_t i = 0;
    while (i < inside.size()) {
        // Gather one baseline line: glyphs whose origin y stays within a fraction
        // of the line's font size of the first glyph's baseline.
        size_t j = i;
        double y0 = inside[i]->y, sz = inside[i]->size;
        double line_l = inside[i]->left, line_r = inside[i]->right;
        double line_b = inside[i]->bottom, line_t = inside[i]->top;
        double covered = 0;
        int nchars = 0;
        while (j < inside.size() && std::fabs(inside[j]->y - y0) <= 0.6 * sz) {
            const CharInfo* c = inside[j];
            line_l = std::min(line_l, c->left);
            line_r = std::max(line_r, c->right);
            line_b = std::min(line_b, c->bottom);
            line_t = std::max(line_t, c->top);
            covered += std::max(0.0, c->right - c->left);
            if (!figure_label_glyph(*c)) ++nchars;
            ++j;
        }
        double span = line_r - line_l;
        double width_frac = region_w > 0 ? span / region_w : 0;
        double fill = span > 0 ? covered / span : 0;
        // Widest single gap between adjacent glyphs on the line (glyphs must be
        // ordered by x, not by stream order, for the gap to be meaningful).
        std::vector<const CharInfo*> line(inside.begin() + i, inside.begin() + j);
        std::sort(line.begin(), line.end(),
                  [](const CharInfo* a, const CharInfo* b) { return a->left < b->left; });
        double max_gap = 0;
        for (size_t g = 1; g < line.size(); ++g)
            max_gap = std::max(max_gap, line[g]->left - line[g - 1]->right);
        // Prose: a real sentence has many ordinary glyphs, reaches across a large
        // fraction of the region, and its glyphs pack the span densely. A row of
        // scattered box labels is wide and can be densely packed too, but its
        // clusters are split by gutters far wider than any word space, so a
        // word-sized largest gap is what separates running text from a label row.
        bool prose = nchars >= 24 && width_frac >= 0.5 && fill >= 0.60 &&
                     max_gap <= kMaxProseGapRatio * sz;
        if (prose) bands.push_back({line_b, line_t});
        i = j;
    }
    return bands;
}

// Counts the PATH objects nested inside a form XObject (recursing through nested
// forms). A vector diagram (UML, flowchart, architecture) draws its boxes, arrows
// and connectors as many nested paths; a form that merely wraps page body text or
// a table's cell fills has few, so the count is the first half of the diagram test.
int count_form_vector_paths(FPDF_PAGEOBJECT form, int depth = 0) {
    int n = 0;
    const int count = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT o = FPDFFormObj_GetObject(form, i);
        if (!o) continue;
        int type = FPDFPageObj_GetType(o);
        if (type == FPDF_PAGEOBJ_PATH) ++n;
        else if (type == FPDF_PAGEOBJ_FORM && depth < 8)
            n += count_form_vector_paths(o, depth + 1);
    }
    return n;
}

// A form's uniform scale factor from its transform matrix. An illustration drawn
// in a tool and imported is authored in its own canvas and placed with a scaling
// transform (factor well below 1); table/body content emitted by the PDF producer
// is drawn at page scale (~1). Combined with a dense nested-path cluster this
// separates a diagram (drop its interior text) from a form-wrapped grid table.
double form_scale(FPDF_PAGEOBJECT form) {
    FS_MATRIX m{1, 0, 0, 1, 0, 0};
    if (!FPDFPageObj_GetMatrix(form, &m)) return 1.0;
    return 0.5 * (std::fabs(m.a) + std::fabs(m.d));
}

// A form holds a vector diagram when it packs many nested paths AND is placed with
// a scaling transform (an imported drawing, not in-page content). The scale gate
// keeps a form-wrapped bordered table -- whose cell rectangles are numerous but
// drawn at page scale -- from being mistaken for a diagram and rasterized away.
constexpr int kMinDiagramVectorPaths = 12;
constexpr double kMaxDiagramFormScale = 0.90;
bool is_diagram_form(FPDF_PAGEOBJECT form) {
    return count_form_vector_paths(form) >= kMinDiagramVectorPaths &&
           form_scale(form) <= kMaxDiagramFormScale;
}

// Figure regions on a page: clusters of vector-graphics object bounds (form
// XObjects and shadings) that are large enough to be a real illustration rather
// than a rule or a fraction bar. A region qualifies either because it does not
// wrap ordinary prose (a plotted figure with degenerate labels) or because it is
// a dense vector diagram (many nested PATH objects) whose scattered ordinary-size
// labels would otherwise fool the prose guard and leak as scrambled text. Raster
// images are handled by the raster path.
std::vector<FigureRegion> detect_figure_regions(FPDF_PAGE page, double page_w, double page_h,
                                                const std::vector<CharInfo>& chars) {
    struct Seed { FigureRect r; bool diagram; };
    std::vector<Seed> seeds;
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj) continue;
        int type = FPDFPageObj_GetType(obj);
        if (type != FPDF_PAGEOBJ_FORM && type != FPDF_PAGEOBJ_SHADING) continue;
        float l = 0, b = 0, r = 0, t = 0;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) continue;
        if (r - l < 4 || t - b < 4) continue;
        seeds.push_back({{l, b, r, t}, type == FPDF_PAGEOBJ_FORM && is_diagram_form(obj)});
    }

    // Greedy clustering of form/shading seeds; a region is a diagram if any form it
    // absorbs is one.
    const double gap = 18.0;
    struct Region { FigureRect r; bool diagram; };
    std::vector<Region> regions;
    std::vector<char> used(seeds.size(), 0);
    for (size_t i = 0; i < seeds.size(); ++i) {
        if (used[i]) continue;
        FigureRect cur = seeds[i].r;
        bool diagram = seeds[i].diagram;
        used[i] = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t j = 0; j < seeds.size(); ++j) {
                if (used[j] || !rects_close(cur, seeds[j].r, gap)) continue;
                cur.left = std::min(cur.left, seeds[j].r.left);
                cur.bottom = std::min(cur.bottom, seeds[j].r.bottom);
                cur.right = std::max(cur.right, seeds[j].r.right);
                cur.top = std::max(cur.top, seeds[j].r.top);
                diagram = diagram || seeds[j].diagram;
                used[j] = 1;
                grew = true;
            }
        }
        regions.push_back({cur, diagram});
    }

    std::vector<FigureRegion> kept;
    for (const Region& rg : regions) {
        // A form whose content bbox grossly overshoots the page is degenerate
        // (off-page or clipped drawing, e.g. a symbol master), not a placed figure;
        // rasterizing it would swallow the whole page's text. Real figures fit the
        // page bar a small overhang, which is clamped back on.
        double w = rg.r.right - rg.r.left, h = rg.r.top - rg.r.bottom;
        if (w > 1.3 * page_w || h > 1.3 * page_h) continue;
        FigureRect box = {std::max(rg.r.left, 0.0), std::max(rg.r.bottom, 0.0),
                          std::min(rg.r.right, page_w), std::min(rg.r.top, page_h)};
        w = box.right - box.left;
        h = box.top - box.bottom;
        if (w < 0.12 * page_w || h < 0.08 * page_h) continue;
        if (!rg.diagram && region_holds_prose(box, chars)) continue;
        kept.push_back({box, rg.diagram});
    }
    return kept;
}

// Trims a figure region's render box so it excludes a real caption or heading
// that only grazes the region's bottom or top edge: such text is emitted as
// markdown, so baking it into the raster too would duplicate it. Only ordinary
// (non-degenerate) glyphs inside the region's horizontal span and within a band
// of an edge move the clip; the figure's interior graphics are untouched.
FigureRect clip_render_box(const FigureRect& rgn, const std::vector<CharInfo>& chars) {
    const double h = rgn.top - rgn.bottom;
    if (h <= 0) return rgn;
    const double band = std::min(0.25 * h, 80.0);
    double new_top = rgn.top, new_bottom = rgn.bottom;
    for (const CharInfo& c : chars) {
        if (c.space || figure_label_glyph(c)) continue;
        double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
        if (cx < rgn.left || cx > rgn.right) continue;
        if (cy <= rgn.bottom || cy >= rgn.top) continue;  // outside the region
        if (cy > rgn.top - band) new_top = std::min(new_top, c.bottom - 2.0);
        else if (cy < rgn.bottom + band) new_bottom = std::max(new_bottom, c.top + 2.0);
    }
    if (new_top <= new_bottom + 4.0) return rgn;  // clip collapsed: keep original
    return {rgn.left, new_bottom, rgn.right, new_top};
}

constexpr double kRegionRenderScale = 2.0;  // ~144 DPI

// Renders one page region into a bitmap sized to the region, offset so the
// region lands at the bitmap origin. Returns null for degenerate regions.
BitmapHandle render_region_bitmap(FPDF_PAGE page, double page_w, double page_h,
                                  const FigureRect& rgn) {
    const double scale = kRegionRenderScale;
    int w = static_cast<int>(std::lround((rgn.right - rgn.left) * scale));
    int h = static_cast<int>(std::lround((rgn.top - rgn.bottom) * scale));
    if (w < 1 || h < 1 || w > 8000 || h > 8000) return nullptr;

    BitmapHandle bitmap(FPDFBitmap_Create(w, h, 0));
    if (!bitmap) return nullptr;
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, w, h, 0xFFFFFFFF);  // opaque white backdrop
    int start_x = static_cast<int>(std::lround(-rgn.left * scale));
    int start_y = static_cast<int>(std::lround(-(page_h - rgn.top) * scale));
    int size_x = static_cast<int>(std::lround(page_w * scale));
    int size_y = static_cast<int>(std::lround(page_h * scale));
    FPDF_RenderPageBitmap(bitmap.get(), page, start_x, start_y, size_x, size_y, 0, 0);
    return bitmap;
}

// Rasterizes one page region to a PNG.
bool render_region_png(FPDF_PAGE page, double page_w, double page_h, const FigureRect& rgn,
                       const std::filesystem::path& out) {
    BitmapHandle bitmap = render_region_bitmap(page, page_w, page_h, rgn);
    return bitmap && write_bitmap_png(bitmap.get(), out);
}

// ------------------------------------------------------- scanned diagrams
//
// In a scanned book the illustrations are baked into the page-scan bitmap that
// the background suppression just dropped, so they would silently vanish from
// the output. They are recovered from the scan itself: any tall text-free band
// between two text lines of the page's column is cropped from the rendered
// page and embedded as a figure. Blank paper is not a figure -- a band is only
// kept when its pixels actually carry ink -- and glyphs stamped in the outer
// margin (a DRM stamp, often level with a diagram) must not split a
// band, so they are ignored when the bands are computed.
constexpr double kMinDiagramBandHeight = 40.0;
constexpr double kMinBandInkFraction = 0.004;
void extract_scan_diagram_bands(FPDF_PAGE page, PageData& page_data,
                                const ExtractOptions& options, int& image_counter,
                                const FigureRect& scan) {
    struct Interval {
        double lo, hi;
    };
    std::vector<Interval> ivals;
    for (const CharInfo& c : page_data.chars) {
        if (c.space) continue;
        if ((c.left + c.right) / 2.0 > 0.90 * page_data.width) continue;  // margin stamps
        ivals.push_back({c.bottom, c.top});
    }
    if (ivals.size() < 2) return;
    std::sort(ivals.begin(), ivals.end(),
              [](const Interval& a, const Interval& b) { return a.lo < b.lo; });

    // Walk the merged coverage bottom-up; every gap between two occupied spans
    // is an interior text-free band.
    std::vector<Interval> gaps;
    double cur_hi = ivals[0].hi;
    for (size_t i = 1; i < ivals.size(); ++i) {
        if (ivals[i].lo > cur_hi + kMinDiagramBandHeight) gaps.push_back({cur_hi, ivals[i].lo});
        cur_hi = std::max(cur_hi, ivals[i].hi);
    }

    for (const Interval& gap : gaps) {
        // The band takes nearly the whole gap -- the OCR boxes sit a few points
        // off the print, so a generous inset would clip the figure; the write
        // instead trims any bled neighbour text line off the edges in pixel
        // space. Horizontally it spans the whole page: a diagram's side
        // annotations reach past the text column, and the scan object's
        // reported bounds can undershoot what it draws. The final crop is the
        // ink bounding box.
        FigureRect band{std::min(scan.left, 0.0), std::max(gap.lo + 2.0, scan.bottom),
                        std::max(scan.right, page_data.width),
                        std::min(gap.hi - 2.0, scan.top)};
        if (band.top - band.bottom < 0.6 * kMinDiagramBandHeight) continue;
        BitmapHandle bitmap =
            render_region_bitmap(page, page_data.width, page_data.height, band);
        if (!bitmap) continue;

        // Blank out any margin stamp level with the band (an on-page DRM mark)
        // so it is neither counted as ink nor baked into the crop.
        for (const CharInfo& c : page_data.chars) {
            if (c.space || (c.left + c.right) / 2.0 <= 0.90 * page_data.width) continue;
            if (c.top < band.bottom || c.bottom > band.top) continue;
            int px = std::max(0, static_cast<int>(std::lround(
                                     (c.left - 2.0 - band.left) * kRegionRenderScale)));
            int py = std::max(0, static_cast<int>(std::lround(
                                     (band.top - c.top - 2.0) * kRegionRenderScale)));
            int pw = static_cast<int>(std::lround((c.right - c.left + 4.0) * kRegionRenderScale));
            int ph = static_cast<int>(std::lround((c.top - c.bottom + 4.0) * kRegionRenderScale));
            FPDFBitmap_FillRect(bitmap.get(), px, py, pw, ph, 0xFFFFFFFF);
        }

        std::string file_name = "image_" + std::to_string(image_counter + 1) + ".png";
        std::filesystem::path out = options.image_dir / file_name;
        if (!write_scan_band_png(bitmap.get(), out, /*pad=*/12, kMinBandInkFraction))
            continue;  // blank paper
        ++image_counter;
        page_data.images.push_back(
            ImageRef{options.image_ref_prefix + file_name, band.left, band.top, band.bottom});
    }
}

// The em-box height of a glyph on the page: FPDFText_GetLooseCharBox scales the
// font's em square through the full text matrix, so for text set with a tiny
// nominal size blown up by the matrix (Tf 1 + a scaling Tm -- the standard shape
// of a scanner's OCR text layer) it reports the size the text is actually drawn
// at, where FPDFText_GetFontSize reports only the nominal 1.0.
double loose_char_height(FPDF_TEXTPAGE tp, int index) {
    FS_RECTF rect{};
    if (!FPDFText_GetLooseCharBox(tp, index, &rect)) return 0.0;
    return static_cast<double>(rect.top) - static_cast<double>(rect.bottom);
}

// OCR text layers (a scanned book with recognized text painted invisibly over
// the page scan) report their nominal (pre-matrix) font size, which wrecks
// every size-derived heuristic downstream: word gaps collapse to fractions of a
// point (spraying spaces inside words), line clustering tolerances shrink to
// nothing, and ordinary watermark text towers over the "1pt" body like a
// heading. When most of a page's glyphs are drawn far larger than their claimed
// size, the page is such a layer, and three repairs are applied:
//   1. each degenerate glyph's size becomes the height it is actually drawn at;
//   2. space glyphs are re-anchored between their stream neighbours -- OCR
//      space glyphs carry sloppy coordinates (often zero-width, overlapping the
//      previous glyph), and the layout stage's x-sort would drag them into the
//      middle of the preceding word ("words of" -> "word sof");
//   3. every char is tagged `ocr` so line building can use OCR-tolerant gap
//      heuristics (the layer packs words to the scan's geometry, where a word
//      gap can be far tighter than any print typography would allow).
// Pages where only a minority of glyphs are degenerate (figure axis labels on a
// normal page) are left alone -- that very degeneracy is the signal
// figure-label suppression keys on.
constexpr double kDegenerateSizeRatio = 1.8;
void repair_ocr_text_layer(PageData& page_data, const std::vector<double>& loose_heights) {
    size_t glyphs = 0, degenerate = 0;
    for (size_t i = 0; i < page_data.chars.size(); ++i) {
        const CharInfo& c = page_data.chars[i];
        if (c.space || c.size <= 0) continue;
        ++glyphs;
        if (loose_heights[i] > kDegenerateSizeRatio * c.size) ++degenerate;
    }
    if (glyphs < 20 || degenerate * 2 <= glyphs) return;

    std::vector<CharInfo>& chars = page_data.chars;
    for (size_t i = 0; i < chars.size(); ++i) {
        CharInfo& c = chars[i];
        c.ocr = true;
        if (c.size > 0 && loose_heights[i] > kDegenerateSizeRatio * c.size)
            c.size = loose_heights[i];
    }

    // Re-anchor each space between its stream neighbours (chars are still in
    // stream order here). Only spaces flanked by two glyphs on the same
    // baseline move; a space at a line wrap keeps its geometry.
    for (size_t i = 1; i + 1 < chars.size(); ++i) {
        CharInfo& sp = chars[i];
        if (!sp.space) continue;
        const CharInfo& prev = chars[i - 1];
        const CharInfo& next = chars[i + 1];
        if (prev.space || next.space) continue;
        double tol = 0.45 * std::max({prev.size, next.size, 1.0});
        if (std::fabs(prev.y - next.y) > tol) continue;
        if (next.left < prev.right - tol) continue;  // not left-to-right adjacent
        sp.left = prev.right;
        sp.right = std::max(next.left, prev.right);
        sp.x = sp.left;
        sp.y = prev.y;
    }
}

void extract_page_chars(FPDF_TEXTPAGE tp, PageData& page_data, const ExtractOptions& options) {
    const int count = FPDFText_CountChars(tp);
    page_data.chars.reserve(static_cast<size_t>(std::max(count, 0)));
    // Em-box height per extracted char, parallel to page_data.chars; feeds the
    // OCR-layer size repair after the page's chars are collected.
    std::vector<double> loose_heights;
    loose_heights.reserve(page_data.chars.capacity());

    // Set PDF2MD_DEBUG_RAW=1 to dump the raw character stream to stderr.
    static const bool debug_raw = std::getenv("PDF2MD_DEBUG_RAW") != nullptr;

    for (int i = 0; i < count; ++i) {
        char32_t cp = static_cast<char32_t>(FPDFText_GetUnicode(tp, i));
        if (debug_raw) {
            double dx = 0, dy = 0;
            FPDFText_GetCharOrigin(tp, i, &dx, &dy);
            std::fprintf(stderr, "raw[%d] U+%04X '%c' x=%.1f y=%.1f\n", i,
                         static_cast<unsigned>(cp),
                         (cp >= 32 && cp < 127) ? static_cast<char>(cp) : '?', dx, dy);
        }

        // PDFium's text page stores UTF-16 internally: recombine surrogate
        // pairs; unpaired surrogates (including one at the last index) are
        // dropped so no invalid code point can reach the output.
        if (cp >= 0xD800 && cp < 0xDC00) {
            char32_t lo = i + 1 < count
                              ? static_cast<char32_t>(FPDFText_GetUnicode(tp, i + 1))
                              : 0;
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                continue;
            }
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            continue;
        }

        // Drop noise and synthesized line breaks; geometry drives line structure.
        if (cp == 0 || cp == 0xFFFF || cp == U'\r' || cp == U'\n') continue;

        // PDFium replaces line-end hyphens it dehyphenated with U+0002 (older
        // versions use U+FFFE). Carry them as soft hyphens so the writer can
        // rejoin the split word.
        bool dehyphenated = cp == 0x0002 || cp == 0xFFFE;
        if (dehyphenated) cp = 0x00AD;

        if (cp == U'\t') cp = U' ';
        else if (cp < 0x20 && !dehyphenated) continue;  // stray control chars

        cp = map_private_use(cp);

        // Font info is needed both to recover math-extension operators and to
        // classify weight/style; fetch it once here and reuse below.
        char font_name_buf[256] = {0};
        int font_flags = 0;
        unsigned long font_got =
            FPDFText_GetFontInfo(tp, i, font_name_buf, sizeof(font_name_buf), &font_flags);
        std::string font_name = font_got > 0 ? std::string(font_name_buf) : std::string();
        cp = map_math_extension_glyph(cp, font_name);

        if (!options.keep_rotated) {
            float angle = FPDFText_GetCharAngle(tp, i);
            if (angle >= 0) {
                double dev = std::fabs(std::remainder(static_cast<double>(angle), 2.0 * 3.14159265358979));
                if (dev > 0.15) continue;  // skip rotated text (watermarks etc.)
            }
        }

        CharInfo ch;
        ch.size = FPDFText_GetFontSize(tp, i);
        if (!FPDFText_GetCharOrigin(tp, i, &ch.x, &ch.y)) continue;
        if (!FPDFText_GetCharBox(tp, i, &ch.left, &ch.right, &ch.bottom, &ch.top)) {
            ch.left = ch.x;
            ch.right = ch.x;
            ch.bottom = ch.y;
            ch.top = ch.y + ch.size;
        }
        if (ch.left > ch.right) std::swap(ch.left, ch.right);
        if (ch.bottom > ch.top) std::swap(ch.bottom, ch.top);

        // A dehyphenation marker is only meaningful right after the word it
        // ended; drop it if it is not on the same baseline as its neighbour.
        if (dehyphenated) {
            if (page_data.chars.empty()) continue;
            const CharInfo& prev = page_data.chars.back();
            double tol = 0.7 * std::max({ch.size, prev.size, 1.0});
            if (std::fabs(prev.y - ch.y) > tol) continue;
        }

        FontProps props = font_props(tp, i, font_name, font_flags);
        ch.bold = props.bold;
        ch.italic = props.italic;
        // CJK fonts legitimately declare fixed pitch; that must not mark
        // ideographic text as code.
        ch.mono = props.mono && !is_cjk_cp(cp);
        ch.space = is_space_cp(cp);
        if (ch.space) cp = U' ';

        if (debug_raw) {
            char nb[256] = {0};
            int fl = 0;
            FPDFText_GetFontInfo(tp, i, nb, sizeof(nb), &fl);
            int wt = FPDFText_GetFontWeight(tp, i);
            std::fprintf(stderr,
                         "DBG U+%04X '%c' sz=%.2f y=%.1f L=%.1f R=%.1f B=%.1f T=%.1f b=%d i=%d m=%d w=%d fl=%d font=%s\n",
                         static_cast<unsigned>(cp),
                         (cp >= 32 && cp < 127) ? static_cast<char>(cp) : '?', ch.size, ch.y,
                         ch.left, ch.right, ch.bottom, ch.top, props.bold, props.italic, props.mono,
                         wt, fl, nb);
        }

        const double loose_h = loose_char_height(tp, i);

        if (const char32_t* expansion = expand_special(cp)) {
            // Split the glyph box evenly across the expanded characters so
            // gap-based word detection still works. Keep a tiny positive
            // advance even for degenerate boxes so x-sorting stays stable.
            size_t n = std::char_traits<char32_t>::length(expansion);
            double width = std::max((ch.right - ch.left) / static_cast<double>(n), 0.01);
            for (size_t k = 0; k < n; ++k) {
                CharInfo part = ch;
                part.code = expansion[k];
                part.left = ch.left + width * static_cast<double>(k);
                part.right = part.left + width;
                part.x = part.left;
                page_data.chars.push_back(part);
                loose_heights.push_back(loose_h);
            }
            continue;
        }

        ch.code = cp;
        page_data.chars.push_back(ch);
        loose_heights.push_back(loose_h);
    }

    repair_ocr_text_layer(page_data, loose_heights);
}

}  // namespace

Extraction extract_pdf(const std::filesystem::path& file, const ExtractOptions& options) {
    ensure_library();

    // Load through memory: FPDF_LoadDocument uses the ANSI code page for
    // paths on Windows and would break on non-ASCII file names.
    std::vector<char> data = read_file(file);

    DocumentHandle doc(FPDF_LoadMemDocument64(
        data.data(), data.size(), options.password.empty() ? nullptr : options.password.c_str()));
    if (!doc) throw PdfError(load_error_message(FPDF_GetLastError()));

    Extraction result;
    result.meta.title = get_meta_text(doc.get(), "Title");
    result.meta.author = get_meta_text(doc.get(), "Author");
    result.meta.page_count = FPDF_GetPageCount(doc.get());

    // The output directory is created lazily by write_bitmap_png on the first
    // PNG actually written, so an image-free document leaves no empty directory.

    int image_counter = 0;
    std::unordered_map<uint64_t, std::string> image_paths;

    for (int p = 0; p < result.meta.page_count; ++p) {
        if (!options.pages.empty() && !options.pages.count(p)) continue;

        PageHandle page(FPDF_LoadPage(doc.get(), p));
        if (!page) continue;

        PageData page_data;
        page_data.index = p;
        page_data.width = FPDF_GetPageWidthF(page.get());
        page_data.height = FPDF_GetPageHeightF(page.get());

        // Scoped so the text page is released right after character extraction,
        // before its page -- exactly where FPDFText_ClosePage ran previously.
        {
            TextPageHandle tp(FPDFText_LoadPage(page.get()));
            if (tp) {
                extract_page_chars(tp.get(), page_data, options);
            }
        }

        std::vector<FigureRect> suppressed_scans;
        if (options.extract_images) {
            extract_page_images(doc.get(), page.get(), page_data, options, image_counter,
                                image_paths, suppressed_scans);
            // A suppressed page scan may carry the page's illustrations inside
            // the bitmap; mine its text-free bands so they survive as figures.
            if (!options.image_dir.empty())
                for (const FigureRect& scan : suppressed_scans)
                    extract_scan_diagram_bands(page.get(), page_data, options, image_counter,
                                               scan);
        }

        // Ruling lines feed the layout stage's bordered-grid table recovery; they
        // are collected regardless of --images (grids exist without raster export).
        extract_page_rules(page.get(), page_data);
        static const bool debug_rules = std::getenv("PDF2MD_DEBUG_RULES") != nullptr;
        if (debug_rules) {
            for (const RuleSegment& r : page_data.rules)
                std::fprintf(stderr, "RULE p%d %s x0=%.1f y0=%.1f x1=%.1f y1=%.1f\n",
                             p + 1, r.horizontal ? "H" : "V", r.x0, r.y0, r.x1, r.y1);
        }

        // Vector-figure handling runs regardless of --images: even without image
        // export the internal labels must be dropped so they cannot corrupt a
        // neighbouring heading. Rendering the region to a PNG is the image step.
        std::vector<FigureRegion> figures =
            detect_figure_regions(page.get(), page_data.width, page_data.height, page_data.chars);
        if (!figures.empty()) {
            // For each diagram region, find the interior text lines that read as
            // prose (a caption or body sentence overhung by the region box). Their
            // glyphs are exempt from erasure so no real text is silently lost; only
            // the diagram's short, scattered interior labels are dropped.
            std::vector<std::vector<std::pair<double, double>>> keep_bands(figures.size());
            for (size_t k = 0; k < figures.size(); ++k)
                if (figures[k].diagram)
                    keep_bands[k] = diagram_prose_bands(figures[k].rect, page_data.chars);

            // Drop interior labels: a plotted figure keeps only its degenerate
            // axis/overlay glyphs, a vector diagram drops every interior glyph
            // (its labels are ordinary-metric and would leak as scrambled text)
            // except those on a prose line. Real captions/headings sit outside the
            // region box, or read as prose inside it, and survive.
            std::erase_if(page_data.chars, [&](const CharInfo& c) {
                double cx = (c.left + c.right) / 2.0, cy = (c.bottom + c.top) / 2.0;
                for (size_t k = 0; k < figures.size(); ++k) {
                    const FigureRegion& f = figures[k];
                    if (cx < f.rect.left || cx > f.rect.right ||
                        cy < f.rect.bottom || cy > f.rect.top)
                        continue;
                    if (f.diagram) {
                        bool prose = false;
                        for (const auto& b : keep_bands[k])
                            if (cy >= b.first && cy <= b.second) { prose = true; break; }
                        if (!prose) return true;
                    } else if (figure_label_glyph(c)) {
                        return true;
                    }
                }
                return false;
            });

            if (options.extract_images && !options.image_dir.empty()) {
                for (const FigureRegion& f : figures) {
                    FigureRect box = clip_render_box(f.rect, page_data.chars);
                    std::string file_name = "image_" + std::to_string(image_counter + 1) + ".png";
                    std::filesystem::path out = options.image_dir / file_name;
                    if (render_region_png(page.get(), page_data.width, page_data.height, box, out)) {
                        ++image_counter;
                        page_data.images.push_back(ImageRef{options.image_ref_prefix + file_name,
                                                            box.left, box.top, box.bottom});
                    }
                }
            }
        }

        // page closes when it leaves this loop iteration's scope (below), after
        // the data it produced has been moved into the result -- before the next
        // page loads and, ultimately, before the document (closed at function
        // exit). No behavior depends on the exact close point; push_back does not
        // touch the page handle.
        result.pages.push_back(std::move(page_data));
    }

    return result;
}

}  // namespace pdf2md
