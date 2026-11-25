#include "pdf_extractor.h"

#include "image_extractor.h"
#include "utf8.h"

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Adapter: this translation unit is the sole adapter between PDFium's C API
// (raw opaque handles, UTF-16LE text buffers, device/page matrices) and the
// domain model in document_model.h (CharInfo / ImageRef / RuleSegment /
// PageData). No PDFium type leaks past this file's boundary.
namespace pdf2md {

namespace {

// Process-wide PDFium lifecycle and serialization guard (Meyers Singleton).
// C++11 static initialization guarantees one-time thread-safe FPDF_InitLibrary,
// and mutex() serializes calls into PDFium's non-thread-safe global C state
// (including FPDF_GetLastError).
class PdfiumInit {
public:
    static PdfiumInit& instance() noexcept {
        static PdfiumInit init;
        return init;
    }

    PdfiumInit(const PdfiumInit&) = delete;
    PdfiumInit& operator=(const PdfiumInit&) = delete;
    PdfiumInit(PdfiumInit&&) = delete;
    PdfiumInit& operator=(PdfiumInit&&) = delete;

    [[nodiscard]] std::mutex& mutex() noexcept { return mutex_; }

private:
    PdfiumInit() noexcept { FPDF_InitLibrary(); }
    ~PdfiumInit() { FPDF_DestroyLibrary(); }

    std::mutex mutex_;
};

// RAII owners for PDFium C handles. Destruction runs in reverse construction
// order, releasing text pages and bitmaps before pages, and pages before the
// document.
struct FpdfDocumentDeleter {
    void operator()(FPDF_DOCUMENT h) const noexcept { FPDF_CloseDocument(h); }
};
struct FpdfPageDeleter {
    void operator()(FPDF_PAGE h) const noexcept { FPDF_ClosePage(h); }
};
struct FpdfTextPageDeleter {
    void operator()(FPDF_TEXTPAGE h) const noexcept { FPDFText_ClosePage(h); }
};
struct FpdfBitmapDeleter {
    void operator()(FPDF_BITMAP h) const noexcept { FPDFBitmap_Destroy(h); }
};
using DocumentHandle = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, FpdfDocumentDeleter>;
using PageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, FpdfPageDeleter>;
using TextPageHandle = std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, FpdfTextPageDeleter>;
using BitmapHandle = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, FpdfBitmapDeleter>;

bool has_env_flag(const char* name) noexcept {
#ifdef _MSC_VER
    char* buf = nullptr;
    std::size_t len = 0U;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
        return false;
    }
    std::free(buf);
    return true;
#else
    return std::getenv(name) != nullptr;
#endif
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::vector<char> read_file(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::is_directory(file, ec)) {
        throw PdfError("cannot open file: " + path_to_utf8(file));
    }
    std::ifstream is(file, std::ios::binary);
    if (!is) {
        throw PdfError("cannot open file: " + path_to_utf8(file));
    }
    is.seekg(0, std::ios::end);
    const std::streamoff len = is.tellg();
    if (len < 0) {
        throw PdfError("cannot read file: " + path_to_utf8(file));
    }
    if (len == 0) {
        throw PdfError("file is not a valid PDF");
    }
    is.seekg(0, std::ios::beg);
    std::vector<char> data(static_cast<std::size_t>(len));
    is.read(data.data(), static_cast<std::streamsize>(len));
    if (!is) {
        throw PdfError("cannot read file: " + path_to_utf8(file));
    }
    return data;
}

std::string load_error_message(unsigned long err) {
    switch (err) {
        case FPDF_ERR_FILE:
            return "file not found or could not be read";
        case FPDF_ERR_FORMAT:
            return "file is not a valid PDF";
        case FPDF_ERR_PASSWORD:
            return "password required or incorrect password (use --password)";
        case FPDF_ERR_SECURITY:
            return "unsupported security scheme";
        case FPDF_ERR_PAGE:
            return "page not found or content error";
        default:
            return "unknown error loading document";
    }
}

std::string get_meta_text(FPDF_DOCUMENT doc, const char* tag) {
    const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= 2UL) {
        return {};
    }
    std::vector<unsigned char> buf(static_cast<std::size_t>(bytes));
    FPDF_GetMetaText(doc, tag, buf.data(), bytes);
    return utf16le_to_utf8(buf.data(), buf.size());
}

bool name_contains(std::string_view haystack, std::string_view needle) noexcept {
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) noexcept {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

// Common ligatures normalized to plain code points.
constexpr std::u32string_view expand_special(char32_t cp) noexcept {
    switch (cp) {
        case 0xFB00U:
            return U"ff";
        case 0xFB01U:
            return U"fi";
        case 0xFB02U:
            return U"fl";
        case 0xFB03U:
            return U"ffi";
        case 0xFB04U:
            return U"ffl";
        case 0xFB05U:
            return U"ft";
        case 0xFB06U:
            return U"st";
        default:
            return {};
    }
}

constexpr char32_t map_private_use(char32_t cp) noexcept {
    switch (cp) {
        case 0xF0B7U:
            return 0x2022U;  // Symbol/Wingdings bullet
        case 0xF0A7U:
            return 0x25AAU;  // Wingdings small square
        case 0xF0D8U:
            return 0x2023U;  // Wingdings arrow bullet
        case 0xF0FCU:
            return 0x2713U;  // Wingdings check mark
        default:
            return cp;
    }
}

// TeX's Computer Modern math-extension font (CMEX) carries large operators
// (summation, product) without a ToUnicode entry, so PDFium decodes them as
// the Latin letter in the same font slot.
char32_t map_math_extension_glyph(char32_t cp, std::string_view font_name) noexcept {
    if (!name_contains(font_name, "cmex")) {
        return cp;
    }
    switch (cp) {
        case U'P':
        case U'X':
            return 0x2211U;  // n-ary summation
        case U'Q':
        case U'Y':
            return 0x220FU;  // n-ary product
        default:
            return cp;
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
constexpr int kBoldWeightThreshold = 600;

FontProps font_props(FPDF_TEXTPAGE tp, int index, std::string_view name, int flags) {
    FontProps props;
    const int weight = FPDFText_GetFontWeight(tp, index);
    // TeX's Computer Modern bold families ("CMBX", "CMBSY", "CMMIB", "CMBXTI")
    // declare a weight around 570 rather than 700 and carry no "bold" in the name.
    props.bold = (weight >= kBoldWeightThreshold) || ((flags & kFontForceBold) != 0) ||
                 name_contains(name, "bold") || name_contains(name, "black") ||
                 name_contains(name, "heavy") || name_contains(name, "cmbx") ||
                 name_contains(name, "cmbsy") || name_contains(name, "cmmib");
    props.italic = ((flags & kFontItalic) != 0) || name_contains(name, "italic") ||
                   name_contains(name, "oblique");
    props.mono = ((flags & kFontFixedPitch) != 0) || name_contains(name, "courier") ||
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
constexpr float kMinObjectExtentPt = 4.0F;
constexpr double kBgMinSpanFraction = 0.60;
constexpr double kBgMinAreaFraction = 0.55;
constexpr std::size_t kBgMinOcrGlyphs = 25U;

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
        std::fprintf(stderr,
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

void extract_page_images(FPDF_DOCUMENT doc, FPDF_PAGE page, PageData& page_data,
                         const ExtractOptions& options, int& image_counter,
                         std::unordered_map<std::uint64_t, std::string>& image_paths,
                         std::vector<FigureRect>& suppressed_scans) {
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
            continue;
        }

        float left = 0.0F;
        float bottom = 0.0F;
        float right = 0.0F;
        float top = 0.0F;
        if (!FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) {
            continue;
        }
        if (right - left < kMinObjectExtentPt || top - bottom < kMinObjectExtentPt) {
            continue;
        }
        if (is_page_background_image(left, bottom, right, top, page_data)) {
            suppressed_scans.push_back({left, bottom, right, top});
            continue;
        }

        BitmapHandle bitmap(FPDFImageObj_GetRenderedBitmap(doc, page, obj));
        if (!bitmap) {
            continue;
        }

        const std::uint64_t hash = hash_bitmap(bitmap.get());
        std::string ref;
        const auto it = image_paths.find(hash);
        if (it != image_paths.end()) {
            ref = it->second;
        } else if (!options.image_dir.empty()) {
            const std::string file_name = "image_" + std::to_string(image_counter + 1) + ".png";
            const std::filesystem::path out = options.image_dir / file_name;
            if (write_bitmap_png(bitmap.get(), out)) {
                ++image_counter;
                ref = options.image_ref_prefix + file_name;
                image_paths.emplace(hash, ref);
            }
        }
        if (!ref.empty()) {
            page_data.images.push_back(ImageRef{std::move(ref), left, top, bottom});
        }
    }
}

// Applies a page-object matrix to a point (path points are in the object's own
// space; the matrix places them on the page).
constexpr void apply_matrix(const FS_MATRIX& m, double x, double y, double& ox,
                            double& oy) noexcept {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

void extract_page_rules(FPDF_PAGE page, PageData& page_data) {
    constexpr double kEps = 1.6;           // max off-axis deviation for a rule
    constexpr double kMinLen = 6.0;        // shortest edge kept
    constexpr double kPageBleedPt = 2.0;   // tolerance outside MediaBox
    const double page_w = page_data.width;
    const double page_h = page_data.height;
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) {
            continue;
        }

        FS_MATRIX m{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        if (!FPDFPageObj_GetMatrix(obj, &m)) {
            m = {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        }

        const int nseg = FPDFPath_CountSegments(obj);
        if (nseg < 2) {
            continue;
        }
        double px = 0.0;
        double py = 0.0;
        double sx = 0.0;
        double sy = 0.0;
        bool have_prev = false;
        for (int s = 0; s < nseg; ++s) {
            FPDF_PATHSEGMENT seg = FPDFPath_GetPathSegment(obj, s);
            if (seg == nullptr) {
                continue;
            }
            float fx = 0.0F;
            float fy = 0.0F;
            if (!FPDFPathSegment_GetPoint(seg, &fx, &fy)) {
                continue;
            }
            double x = 0.0;
            double y = 0.0;
            apply_matrix(m, fx, fy, x, y);
            const int type = FPDFPathSegment_GetType(seg);

            const auto emit_edge = [&](double ax, double ay, double bx, double by) {
                const double dx = std::fabs(bx - ax);
                const double dy = std::fabs(by - ay);
                if (dy <= kEps && dx >= kMinLen) {
                    const double yy = (ay + by) / 2.0;
                    page_data.rules.push_back(
                        {(std::min)(ax, bx), yy, (std::max)(ax, bx), yy, true});
                    return;
                }
                if (dx <= kEps && dy >= kMinLen) {
                    const double xx = (ax + bx) / 2.0;
                    page_data.rules.push_back(
                        {xx, (std::min)(ay, by), xx, (std::max)(ay, by), false});
                }
            };

            if (type == FPDF_SEGMENT_MOVETO) {
                px = sx = x;
                py = sy = y;
                have_prev = true;
            } else if (type == FPDF_SEGMENT_LINETO) {
                if (have_prev) {
                    emit_edge(px, py, x, y);
                }
                px = x;
                py = y;
                have_prev = true;
            } else {
                px = x;
                py = y;
                have_prev = true;
            }
            if (FPDFPathSegment_GetClose(seg) && have_prev) {
                emit_edge(x, y, sx, sy);
            }
        }
    }
    std::erase_if(page_data.rules, [&](const RuleSegment& r) noexcept {
        return r.x1 < -kPageBleedPt || r.y1 < -kPageBleedPt || r.x0 > page_w + kPageBleedPt ||
               r.y0 > page_h + kPageBleedPt;
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

constexpr bool rects_close(const FigureRect& a, const FigureRect& b, double gap) noexcept {
    return a.left - gap <= b.right && b.left - gap <= a.right &&
           a.bottom - gap <= b.top && b.bottom - gap <= a.top;
}

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

constexpr double kProseEdgeInsetRatio = 0.15;
constexpr double kProseMaxEdgeInsetPt = 40.0;
constexpr std::size_t kMinInteriorProseGlyphs = 25U;

bool region_holds_prose(const FigureRect& r, const std::vector<CharInfo>& chars) noexcept {
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

constexpr double kBaselineClusterRatio = 0.6;
constexpr int kMinProseLineChars = 24;
constexpr double kMinProseWidthFraction = 0.5;
constexpr double kMinProseFillFraction = 0.60;
constexpr double kMaxProseGapRatio = 1.5;

std::vector<std::pair<double, double>> diagram_prose_bands(
        const FigureRect& r, const std::vector<CharInfo>& chars) {
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

constexpr int kMaxFormNestingDepth = 8;

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

constexpr int kMinDiagramVectorPaths = 12;
constexpr double kMaxDiagramFormScale = 0.90;

bool is_diagram_form(FPDF_PAGEOBJECT form) {
    return count_form_vector_paths(form) >= kMinDiagramVectorPaths &&
           form_scale(form) <= kMaxDiagramFormScale;
}

constexpr double kFigureClusterGapPt = 18.0;
constexpr double kMaxFigureOvershootRatio = 1.3;
constexpr double kMinFigureWidthRatio = 0.12;
constexpr double kMinFigureHeightRatio = 0.08;

std::vector<FigureRegion> detect_figure_regions(FPDF_PAGE page, double page_w, double page_h,
                                                const std::vector<CharInfo>& chars) {
    struct Seed {
        FigureRect r;
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
        FigureRect cur = seeds[i].r;
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
        const FigureRect box = {(std::max)(rg.rect.left, 0.0), (std::max)(rg.rect.bottom, 0.0),
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

constexpr double kClipEdgeBandRatio = 0.25;
constexpr double kClipMaxEdgeBandPt = 80.0;
constexpr double kClipGlyphClearancePt = 2.0;
constexpr double kMinClippedHeightPt = 4.0;

FigureRect clip_render_box(const FigureRect& rgn, const std::vector<CharInfo>& chars) noexcept {
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

constexpr double kRegionRenderScale = 2.0;  // ~144 DPI
constexpr int kMaxRenderDimPx = 8000;
constexpr unsigned long kOpaqueWhiteArgb = 0xFFFFFFFFUL;

BitmapHandle render_region_bitmap(FPDF_PAGE page, double page_w, double page_h,
                                  const FigureRect& rgn) {
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

bool render_region_png(FPDF_PAGE page, double page_w, double page_h, const FigureRect& rgn,
                       const std::filesystem::path& out) {
    const BitmapHandle bitmap = render_region_bitmap(page, page_w, page_h, rgn);
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
constexpr double kOuterMarginStampRatio = 0.90;
constexpr double kBandGapInsetPt = 2.0;
constexpr double kMinTrimmedBandRatio = 0.6;
constexpr int kScanBandPadPx = 12;

void extract_scan_diagram_bands(FPDF_PAGE page, PageData& page_data,
                                const ExtractOptions& options, int& image_counter,
                                const FigureRect& scan) {
    struct Interval {
        double lo = 0.0;
        double hi = 0.0;
    };
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
        const FigureRect band{(std::min)(scan.left, 0.0),
                              (std::max)(gap.lo + kBandGapInsetPt, scan.bottom),
                              (std::max)(scan.right, page_data.width),
                              (std::min)(gap.hi - kBandGapInsetPt, scan.top)};
        if (band.top - band.bottom < kMinTrimmedBandRatio * kMinDiagramBandHeight) {
            continue;
        }
        BitmapHandle bitmap =
            render_region_bitmap(page, page_data.width, page_data.height, band);
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

        const std::string file_name = "image_" + std::to_string(image_counter + 1) + ".png";
        const std::filesystem::path out = options.image_dir / file_name;
        if (!write_scan_band_png(bitmap.get(), out, kScanBandPadPx, kMinBandInkFraction)) {
            continue;
        }
        ++image_counter;
        page_data.images.push_back(
            ImageRef{options.image_ref_prefix + file_name, band.left, band.top, band.bottom});
    }
}

double loose_char_height(FPDF_TEXTPAGE tp, int index) {
    FS_RECTF rect{};
    if (!FPDFText_GetLooseCharBox(tp, index, &rect)) {
        return 0.0;
    }
    const double h = static_cast<double>(rect.top) - static_cast<double>(rect.bottom);
    return std::isfinite(h) ? h : 0.0;
}

constexpr double kInflatedSizeRatio = 1.8;
constexpr std::size_t kMinRepairGlyphs = 20U;

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

constexpr double kDegenerateSizeRatio = 1.8;
constexpr double kOcrBaselineTolRatio = 0.45;

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

constexpr char32_t kPdfiumDehyphenCtrl = 0x0002U;
constexpr char32_t kPdfiumDehyphenLegacy = 0xFFFEU;
constexpr char32_t kSoftHyphen = 0x00ADU;
constexpr char32_t kNonCharacterFfff = 0xFFFFU;
constexpr char32_t kAsciiControlEnd = 0x20U;
constexpr char32_t kPrintableAsciiEnd = 0x7FU;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kMaxHorizontalAngleRad = 0.15;
constexpr double kDehyphenBaselineTolRatio = 0.7;
constexpr double kMinExpandedCharWidthPt = 0.01;
constexpr std::size_t kFontNameBufSize = 256U;

void extract_page_chars(FPDF_TEXTPAGE tp, PageData& page_data, const ExtractOptions& options) {
    const int count = FPDFText_CountChars(tp);
    if (count <= 0) {
        return;
    }
    page_data.chars.reserve(static_cast<std::size_t>(count));

    static const bool debug_raw = has_env_flag("PDF2MD_DEBUG_RAW");

    char font_name_buf[kFontNameBufSize];
    for (int i = 0; i < count; ++i) {
        char32_t cp = static_cast<char32_t>(FPDFText_GetUnicode(tp, i));
        if (debug_raw) {
            double dx = 0.0;
            double dy = 0.0;
            FPDFText_GetCharOrigin(tp, i, &dx, &dy);
            std::fprintf(stderr, "raw[%d] U+%04X '%c' x=%.1f y=%.1f\n", i,
                         static_cast<unsigned>(cp),
                         (cp >= kAsciiControlEnd && cp < kPrintableAsciiEnd)
                             ? static_cast<char>(cp)
                             : '?',
                         dx, dy);
        }

        // PDFium's text page stores UTF-16 internally: recombine surrogate pairs
        // and drop unpaired surrogates.
        if (cp >= detail::kSurrogateHighStart && cp <= detail::kSurrogateHighEnd) {
            const char32_t lo = i + 1 < count
                                    ? static_cast<char32_t>(FPDFText_GetUnicode(tp, i + 1))
                                    : 0U;
            if (lo < detail::kSurrogateLowStart || lo > detail::kSurrogateLowEnd) {
                continue;
            }
            cp = detail::kFourByteMin +
                 ((cp - detail::kSurrogateHighStart) << detail::kSurrogateShift) +
                 (lo - detail::kSurrogateLowStart);
            ++i;
        } else if (cp >= detail::kSurrogateLowStart && cp <= detail::kSurrogateLowEnd) {
            continue;
        }

        if (cp == 0U || cp == kNonCharacterFfff || cp == U'\r' || cp == U'\n') {
            continue;
        }

        const bool dehyphenated = cp == kPdfiumDehyphenCtrl || cp == kPdfiumDehyphenLegacy;
        if (dehyphenated) {
            cp = kSoftHyphen;
        }

        if (cp == U'\t') {
            cp = U' ';
        } else if (cp < kAsciiControlEnd && !dehyphenated) {
            continue;
        }

        if (!options.keep_rotated) {
            const float angle = FPDFText_GetCharAngle(tp, i);
            if (angle >= 0.0F && std::isfinite(angle)) {
                const double dev = std::fabs(std::remainder(static_cast<double>(angle), kTwoPi));
                if (dev > kMaxHorizontalAngleRad) {
                    continue;
                }
            }
        }

        cp = map_private_use(cp);

        font_name_buf[0] = '\0';
        int font_flags = 0;
        const unsigned long font_got =
            FPDFText_GetFontInfo(tp, i, font_name_buf, sizeof(font_name_buf), &font_flags);
        font_name_buf[kFontNameBufSize - 1U] = '\0';
        const std::string_view font_name =
            font_got > 0UL ? std::string_view(font_name_buf) : std::string_view{};
        cp = map_math_extension_glyph(cp, font_name);

        CharInfo ch;
        ch.size = FPDFText_GetFontSize(tp, i);
        if (!std::isfinite(ch.size)) {
            continue;
        }
        if (!FPDFText_GetCharOrigin(tp, i, &ch.x, &ch.y) || !std::isfinite(ch.x) ||
            !std::isfinite(ch.y)) {
            continue;
        }
        if (!FPDFText_GetCharBox(tp, i, &ch.left, &ch.right, &ch.bottom, &ch.top) ||
            !std::isfinite(ch.left) || !std::isfinite(ch.right) || !std::isfinite(ch.bottom) ||
            !std::isfinite(ch.top)) {
            ch.left = ch.x;
            ch.right = ch.x;
            ch.bottom = ch.y;
            ch.top = ch.y + ch.size;
        }
        if (ch.left > ch.right) {
            std::swap(ch.left, ch.right);
        }
        if (ch.bottom > ch.top) {
            std::swap(ch.bottom, ch.top);
        }

        if (dehyphenated) {
            if (page_data.chars.empty()) {
                continue;
            }
            const CharInfo& prev = page_data.chars.back();
            const double tol = kDehyphenBaselineTolRatio * std::max({ch.size, prev.size, 1.0});
            if (std::fabs(prev.y - ch.y) > tol) {
                continue;
            }
        }

        const FontProps props = font_props(tp, i, font_name, font_flags);
        ch.bold = props.bold;
        ch.italic = props.italic;
        ch.mono = props.mono && !is_cjk_cp(cp);
        ch.space = is_space_cp(cp);
        if (ch.space) {
            cp = U' ';
        }

        ch.loose_height = loose_char_height(tp, i);

        if (debug_raw) {
            const int wt = FPDFText_GetFontWeight(tp, i);
            std::fprintf(stderr,
                         "DBG U+%04X '%c' sz=%.2f loose=%.2f y=%.1f L=%.1f R=%.1f B=%.1f T=%.1f b=%d i=%d m=%d w=%d fl=%d font=%.*s\n",
                         static_cast<unsigned>(cp),
                         (cp >= kAsciiControlEnd && cp < kPrintableAsciiEnd)
                             ? static_cast<char>(cp)
                             : '?',
                         ch.size, ch.loose_height, ch.y, ch.left, ch.right, ch.bottom, ch.top,
                         props.bold, props.italic, props.mono, wt, font_flags,
                         static_cast<int>(font_name.size()), font_name.data());
        }

        const std::u32string_view expansion = expand_special(cp);
        if (!expansion.empty()) {
            const std::size_t n = expansion.size();
            const double width =
                (std::max)((ch.right - ch.left) / static_cast<double>(n), kMinExpandedCharWidthPt);
            for (std::size_t k = 0U; k < n; ++k) {
                CharInfo part = ch;
                part.code = expansion[k];
                part.left = ch.left + width * static_cast<double>(k);
                part.right = part.left + width;
                part.x = part.left;
                page_data.chars.push_back(part);
            }
            continue;
        }

        ch.code = cp;
        page_data.chars.push_back(ch);
    }

    repair_ocr_text_layer(page_data);
}

}  // namespace

Extraction extract_pdf(const std::filesystem::path& file, const ExtractOptions& options) {
    // Perform file I/O outside the process-wide PDFium mutex.
    const std::vector<char> data = read_file(file);

    Extraction result;
    {
        std::lock_guard<std::mutex> lock(PdfiumInit::instance().mutex());

        DocumentHandle doc(FPDF_LoadMemDocument64(
            data.data(), data.size(),
            options.password.empty() ? nullptr : options.password.c_str()));
        if (!doc) {
            throw PdfError(load_error_message(FPDF_GetLastError()));
        }

        result.meta.title = get_meta_text(doc.get(), "Title");
        result.meta.author = get_meta_text(doc.get(), "Author");
        result.meta.page_count = FPDF_GetPageCount(doc.get());

        const std::size_t expected_pages =
            options.pages.empty() ? static_cast<std::size_t>((std::max)(result.meta.page_count, 0))
                                  : options.pages.size();
        result.pages.reserve(expected_pages);

        int image_counter = 0;
        std::unordered_map<std::uint64_t, std::string> image_paths;

        for (int p = 0; p < result.meta.page_count; ++p) {
            if (!options.pages.empty() && options.pages.find(p) == options.pages.end()) {
                continue;
            }

            PageHandle page(FPDF_LoadPage(doc.get(), p));
            if (!page) {
                continue;
            }

            PageData page_data;
            page_data.index = p;
            page_data.width = FPDF_GetPageWidthF(page.get());
            page_data.height = FPDF_GetPageHeightF(page.get());

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
                if (!options.image_dir.empty()) {
                    for (const FigureRect& scan : suppressed_scans) {
                        extract_scan_diagram_bands(page.get(), page_data, options, image_counter,
                                                   scan);
                    }
                }
            }

            extract_page_rules(page.get(), page_data);
            static const bool debug_rules = has_env_flag("PDF2MD_DEBUG_RULES");
            if (debug_rules) {
                for (const RuleSegment& r : page_data.rules) {
                    std::fprintf(stderr, "RULE p%d %s x0=%.1f y0=%.1f x1=%.1f y1=%.1f\n",
                                 p + 1, r.horizontal ? "H" : "V", r.x0, r.y0, r.x1, r.y1);
                }
            }

            const std::vector<FigureRegion> figures = detect_figure_regions(
                page.get(), page_data.width, page_data.height, page_data.chars);
            if (!figures.empty()) {
                std::vector<std::vector<std::pair<double, double>>> keep_bands(figures.size());
                for (std::size_t k = 0U; k < figures.size(); ++k) {
                    if (figures[k].diagram) {
                        keep_bands[k] = diagram_prose_bands(figures[k].rect, page_data.chars);
                    }
                }

                std::erase_if(page_data.chars, [&](const CharInfo& c) noexcept {
                    const double cx = (c.left + c.right) / 2.0;
                    const double cy = (c.bottom + c.top) / 2.0;
                    for (std::size_t k = 0U; k < figures.size(); ++k) {
                        const FigureRegion& f = figures[k];
                        if (cx < f.rect.left || cx > f.rect.right ||
                            cy < f.rect.bottom || cy > f.rect.top) {
                            continue;
                        }
                        if (f.diagram) {
                            const bool prose = std::any_of(
                                keep_bands[k].begin(), keep_bands[k].end(),
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

                if (options.extract_images && !options.image_dir.empty()) {
                    for (const FigureRegion& f : figures) {
                        const FigureRect box = clip_render_box(f.rect, page_data.chars);
                        const std::string file_name =
                            "image_" + std::to_string(image_counter + 1) + ".png";
                        const std::filesystem::path out = options.image_dir / file_name;
                        if (render_region_png(page.get(), page_data.width, page_data.height, box,
                                              out)) {
                            ++image_counter;
                            page_data.images.push_back(ImageRef{
                                options.image_ref_prefix + file_name, box.left, box.top,
                                box.bottom});
                        }
                    }
                }
            }

            result.pages.push_back(std::move(page_data));
        }
    }

    repair_inflated_font_sizes(result.pages);
    return result;
}

}  // namespace pdf2md
