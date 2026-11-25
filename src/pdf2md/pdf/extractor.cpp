#include "pdf2md/pdf_extractor.h"

#include "pdf2md/model/geometry.h"
#include "pdf2md/pdf/figures.h"
#include "pdf2md/pdf/ocr_repair.h"
#include "pdf2md/pdf/pdfium_raii.h"
#include "pdf2md/utf8.h"

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

// Adapter: this translation unit (together with pdf2md/pdf/figures.cpp and
// pdf2md/pdf/pdfium_raii.h) adapts PDFium's C API to the domain model in
// pdf2md/document_model.h. No PDFium type leaks into any public header.
namespace pdf2md::pdf {

using model::Rect;
using model::has_env_flag;

namespace {

constexpr unsigned long kEmptyUtf16PayloadBytes = 2UL;
constexpr unsigned long kEmptyAsciiPayloadBytes = 1UL;
constexpr int kMinWebLinkCodeUnits = 2;
constexpr char32_t kAsciiControlEnd = 0x20U;
constexpr char32_t kPrintableAsciiEnd = 0x7FU;
constexpr char32_t kFourByteMin = 0x10000U;
constexpr char32_t kSurrogateHighStart = 0xD800U;
constexpr char32_t kSurrogateHighEnd = 0xDBFFU;
constexpr char32_t kSurrogateLowStart = 0xDC00U;
constexpr char32_t kSurrogateLowEnd = 0xDFFFU;
constexpr char32_t kSurrogateShift = 10U;

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

// Normalizes Unicode line/paragraph separators (U+2028/U+2029, CR/LF/NEL) to
// ASCII spaces in metadata and bookmark strings while preserving other bytes.
constexpr unsigned char kUtf8NelLead = 0xC2U;
constexpr unsigned char kUtf8NelTrail = 0x85U;
constexpr unsigned char kUtf8SepLead = 0xE2U;
constexpr unsigned char kUtf8SepMid = 0x80U;
constexpr unsigned char kUtf8LineSepTrail = 0xA8U;
constexpr unsigned char kUtf8ParaSepTrail = 0xA9U;

std::string normalize_inline_separators(std::string_view input, bool trim_edges) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0U; i < input.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        if (b0 == '\r' || b0 == '\n') {
            if (out.empty() || out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        if (b0 == kUtf8NelLead && i + 1U < input.size() &&
            static_cast<unsigned char>(input[i + 1U]) == kUtf8NelTrail) {
            if (out.empty() || out.back() != ' ') {
                out.push_back(' ');
            }
            ++i;
            continue;
        }
        if (b0 == kUtf8SepLead && i + 2U < input.size() &&
            static_cast<unsigned char>(input[i + 1U]) == kUtf8SepMid) {
            const auto b2 = static_cast<unsigned char>(input[i + 2U]);
            if (b2 == kUtf8LineSepTrail || b2 == kUtf8ParaSepTrail) {
                if (out.empty() || out.back() != ' ') {
                    out.push_back(' ');
                }
                i += 2U;
                continue;
            }
        }
        if (trim_edges && b0 == '\t') {
            if (out.empty() || out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        out.push_back(static_cast<char>(b0));
    }
    if (!trim_edges) {
        return out;
    }
    std::size_t start = 0U;
    while (start < out.size() && out[start] == ' ') {
        ++start;
    }
    std::size_t end = out.size();
    while (end > start && out[end - 1U] == ' ') {
        --end;
    }
    return out.substr(start, end - start);
}

std::string utf16_units_to_utf8(std::span<const unsigned short> units) {
    std::string out;
    out.reserve(units.size());
    for (std::size_t i = 0U; i < units.size(); ++i) {
        const auto u = static_cast<char32_t>(units[i]);
        if (u == 0U) {
            break;
        }
        if (u >= kSurrogateHighStart && u <= kSurrogateHighEnd) {
            if (i + 1U >= units.size()) {
                continue;
            }
            const auto lo = static_cast<char32_t>(units[i + 1U]);
            if (lo < kSurrogateLowStart || lo > kSurrogateLowEnd) {
                continue;
            }
            const char32_t combined = kFourByteMin +
                                      ((u - kSurrogateHighStart) << kSurrogateShift) +
                                      (lo - kSurrogateLowStart);
            ++i;
            append_utf8(out, combined);
            continue;
        }
        if (u >= kSurrogateLowStart && u <= kSurrogateLowEnd) {
            continue;
        }
        append_utf8(out, u);
    }
    return out;
}

std::string get_meta_text(FPDF_DOCUMENT doc, const char* tag) {
    const unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0UL);
    if (bytes <= kEmptyUtf16PayloadBytes) {
        return {};
    }
    std::vector<unsigned char> buf(static_cast<std::size_t>(bytes));
    FPDF_GetMetaText(doc, tag, buf.data(), bytes);
    return normalize_inline_separators(utf16le_to_utf8(buf.data(), buf.size()), false);
}

constexpr std::size_t kYearDigits = 4U;
constexpr std::size_t kTwoDigitField = 2U;
constexpr std::size_t kIsoDatePrefixLen = 10U;
constexpr std::size_t kIsoFirstDashPos = 4U;
constexpr std::size_t kIsoSecondDashPos = 7U;
constexpr int kDecimalRadix = 10;
constexpr int kMinMonth = 1;
constexpr int kMaxMonth = 12;
constexpr int kMinDay = 1;
constexpr int kMaxDay = 31;
constexpr int kMaxHour = 23;
constexpr int kMaxMinute = 59;
constexpr int kMaxSecond = 60;

bool parse_decimal_digits(std::string_view sv, std::size_t pos, std::size_t len,
                          int& out) noexcept {
    if (pos + len > sv.size()) {
        return false;
    }
    int val = 0;
    for (std::size_t i = 0U; i < len; ++i) {
        const char ch = sv[pos + i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        val = val * kDecimalRadix + (ch - '0');
    }
    out = val;
    return true;
}

void append_two_digits(std::string& out, int value) {
    out.push_back(static_cast<char>('0' + (value / kDecimalRadix) % kDecimalRadix));
    out.push_back(static_cast<char>('0' + (value % kDecimalRadix)));
}

// Normalizes PDF 32000-1 ASN.1 dates ("D:YYYYMMDDHHmmSSOHH'mm'") to ISO-8601
// ("YYYY-MM-DDTHH:MM:SSZ" / "+HH:MM" / "YYYY-MM-DD").
std::string normalize_pdf_date(std::string_view raw) {
    while (!raw.empty() &&
           (raw.front() == '\0' || std::isspace(static_cast<unsigned char>(raw.front())) != 0)) {
        raw.remove_prefix(1U);
    }
    while (!raw.empty() &&
           (raw.back() == '\0' || std::isspace(static_cast<unsigned char>(raw.back())) != 0)) {
        raw.remove_suffix(1U);
    }
    if (raw.empty()) {
        return {};
    }

    // Preserve strings already formatted as ISO-8601.
    int dummy = 0;
    if (raw.size() >= kIsoDatePrefixLen && raw[kIsoFirstDashPos] == '-' &&
        raw[kIsoSecondDashPos] == '-' && parse_decimal_digits(raw, 0U, kYearDigits, dummy)) {
        return std::string(raw);
    }

    std::string_view s = raw;
    if (s.starts_with("D:") || s.starts_with("d:")) {
        s.remove_prefix(2U);
        while (!s.empty() && s.front() == ' ') {
            s.remove_prefix(1U);
        }
    }

    int year = 0;
    if (!parse_decimal_digits(s, 0U, kYearDigits, year)) {
        return std::string(raw);
    }
    std::size_t pos = kYearDigits;

    int month = kMinMonth;
    if (pos + kTwoDigitField <= s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
        if (!parse_decimal_digits(s, pos, kTwoDigitField, month) || month < kMinMonth ||
            month > kMaxMonth) {
            return std::string(raw);
        }
        pos += kTwoDigitField;
    }

    int day = kMinDay;
    if (pos + kTwoDigitField <= s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
        if (!parse_decimal_digits(s, pos, kTwoDigitField, day) || day < kMinDay || day > kMaxDay) {
            return std::string(raw);
        }
        pos += kTwoDigitField;
    }

    std::string iso(s.substr(0U, kYearDigits));
    iso.push_back('-');
    append_two_digits(iso, month);
    iso.push_back('-');
    append_two_digits(iso, day);

    if (pos >= s.size()) {
        return iso;
    }

    int hour = 0;
    if (!parse_decimal_digits(s, pos, kTwoDigitField, hour) || hour > kMaxHour) {
        return iso;
    }
    pos += kTwoDigitField;

    int minute = 0;
    if (pos + kTwoDigitField <= s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
        if (!parse_decimal_digits(s, pos, kTwoDigitField, minute) || minute > kMaxMinute) {
            return std::string(raw);
        }
        pos += kTwoDigitField;
    }

    int second = 0;
    if (pos + kTwoDigitField <= s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
        if (!parse_decimal_digits(s, pos, kTwoDigitField, second) || second > kMaxSecond) {
            return std::string(raw);
        }
        pos += kTwoDigitField;
    }

    iso.push_back('T');
    append_two_digits(iso, hour);
    iso.push_back(':');
    append_two_digits(iso, minute);
    iso.push_back(':');
    append_two_digits(iso, second);

    while (pos < s.size() && s[pos] == ' ') {
        ++pos;
    }
    if (pos >= s.size()) {
        return iso;
    }

    const char tz_lead = s[pos];
    if (tz_lead == 'Z' || tz_lead == 'z') {
        iso.push_back('Z');
        return iso;
    }
    if (tz_lead == '+' || tz_lead == '-') {
        ++pos;
        int tz_hour = 0;
        if (!parse_decimal_digits(s, pos, kTwoDigitField, tz_hour) || tz_hour > kMaxHour) {
            return iso;
        }
        pos += kTwoDigitField;
        if (pos < s.size() && (s[pos] == '\'' || s[pos] == ':')) {
            ++pos;
        }
        int tz_min = 0;
        if (pos + kTwoDigitField <= s.size() &&
            std::isdigit(static_cast<unsigned char>(s[pos])) != 0) {
            if (!parse_decimal_digits(s, pos, kTwoDigitField, tz_min) || tz_min > kMaxMinute) {
                return iso;
            }
        }
        iso.push_back(tz_lead);
        append_two_digits(iso, tz_hour);
        iso.push_back(':');
        append_two_digits(iso, tz_min);
    }
    return iso;
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

bool starts_with_icase(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string sanitize_link_uri(std::string_view raw) {
    if (const std::size_t nul = raw.find('\0'); nul != std::string_view::npos) {
        raw = raw.substr(0U, nul);
    }
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (const char ch : raw) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < static_cast<unsigned char>(kAsciiControlEnd) ||
            b == static_cast<unsigned char>(kPrintableAsciiEnd)) {
            continue;
        }
        cleaned.push_back(ch);
    }
    std::size_t start = 0U;
    while (start < cleaned.size() && cleaned[start] == ' ') {
        ++start;
    }
    std::size_t end = cleaned.size();
    while (end > start && cleaned[end - 1U] == ' ') {
        --end;
    }
    if (end <= start) {
        return {};
    }
    std::string uri = cleaned.substr(start, end - start);
    if (starts_with_icase(uri, "javascript:") || starts_with_icase(uri, "vbscript:") ||
        starts_with_icase(uri, "data:")) {
        return {};
    }
    return uri;
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

constexpr char32_t kWingdingsBallotBox1 = 0xF06FU;
constexpr char32_t kWingdingsBallotBox2 = 0xF0A8U;
constexpr char32_t kWingdingsSmallSquare = 0xF0A7U;
constexpr char32_t kWingdingsBullet = 0xF0B7U;
constexpr char32_t kWingdingsArrowBullet = 0xF0D8U;
constexpr char32_t kWingdingsChecked1 = 0xF0FCU;
constexpr char32_t kWingdingsChecked2 = 0xF0FDU;
constexpr char32_t kWingdingsChecked3 = 0xF0FEU;
constexpr char32_t kUnicodeBallotBoxCp = 0x2610U;
constexpr char32_t kUnicodeBallotCheckCp = 0x2611U;

constexpr char32_t map_private_use(char32_t cp) noexcept {
    switch (cp) {
        case kWingdingsBullet:
            return 0x2022U;  // bullet
        case kWingdingsSmallSquare:
            return 0x25AAU;  // black small square
        case kWingdingsArrowBullet:
            return 0x2023U;  // triangular bullet
        case kWingdingsBallotBox1:
        case kWingdingsBallotBox2:
            return kUnicodeBallotBoxCp;  // ballot box
        case kWingdingsChecked1:
        case kWingdingsChecked2:
        case kWingdingsChecked3:
            return kUnicodeBallotCheckCp;  // ballot box with check
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

bool is_cjk_font_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    constexpr std::array<std::string_view, 15> kCjkFamilies = {
        "mincho", "gothic", "meiryo",   "yu",    "msgothic", "msmincho", "heiti", "song",
        "fangsong", "kaiti", "ming", "batang", "dotum",    "gulim",    "malgun",
    };
    return std::any_of(kCjkFamilies.begin(), kCjkFamilies.end(),
                       [name](std::string_view fam) noexcept { return name_contains(name, fam); });
}

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
                   name_contains(name, "oblique") || name_contains(name, "cmti") ||
                   name_contains(name, "cmbxti") || name_contains(name, "cmsl") ||
                   name_contains(name, "lmti");
    props.mono = ((flags & kFontFixedPitch) != 0) || name_contains(name, "courier") ||
                 name_contains(name, "mono") || name_contains(name, "consol") ||
                 name_contains(name, "cmtt") || name_contains(name, "cmsltt") ||
                 name_contains(name, "inconsolata") || name_contains(name, "beramono") ||
                 name_contains(name, "txtt") || name_contains(name, "lmtt");
    return props;
}

// Applies a page-object matrix to a point (path points are in the object's own
// space; the matrix places them on the page).
constexpr void apply_matrix(const FS_MATRIX& m, double x, double y, double& ox,
                            double& oy) noexcept {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

void extract_page_rules(FPDF_PAGE page, PageData& page_data) {
    constexpr double kEps = 1.6;                       // max off-axis deviation for a rule
    constexpr double kMinLen = 3.0;                    // shortest edge kept (incl. \frac/\sqrt)
    constexpr double kMaxFilledRuleThicknessPt = 3.0;  // max minor dim for unstroked filled rule
    constexpr double kThinStrokedBoxPt = 1.5;          // thin closed stroked box collapsed to 1 rule
    constexpr int kMinPathSegments = 2;
    constexpr int kMaxRectPathSegments = 5;
    constexpr double kPageBleedPt = 2.0;               // tolerance outside MediaBox
    const double page_w = page_data.width;
    const double page_h = page_data.height;
    const int count = FPDFPage_CountObjects(page);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) {
            continue;
        }

        int fillmode = FPDF_FILLMODE_NONE;
        FPDF_BOOL stroke = 0;
        if (!FPDFPath_GetDrawMode(obj, &fillmode, &stroke)) {
            continue;
        }
        if (stroke == 0 && fillmode == FPDF_FILLMODE_NONE) {
            continue;
        }

        const int nseg = FPDFPath_CountSegments(obj);
        if (nseg < kMinPathSegments) {
            continue;
        }

        // Unstroked filled paths (such as TeX \frac bars, \sqrt vinculums, and
        // thin filled rules) emit a single centerline rule only when thin; large
        // filled background rectangles are skipped instead of emitting 4 borders.
        if (stroke == 0) {
            float bl = 0.0F;
            float bb = 0.0F;
            float br = 0.0F;
            float bt = 0.0F;
            if (!FPDFPageObj_GetBounds(obj, &bl, &bb, &br, &bt) || !std::isfinite(bl) ||
                !std::isfinite(bb) || !std::isfinite(br) || !std::isfinite(bt)) {
                continue;
            }
            const double bl_d = static_cast<double>(bl);
            const double br_d = static_cast<double>(br);
            const double bb_d = static_cast<double>(bb);
            const double bt_d = static_cast<double>(bt);
            const double left = (std::min)(bl_d, br_d);
            const double right = (std::max)(bl_d, br_d);
            const double bottom = (std::min)(bb_d, bt_d);
            const double top = (std::max)(bb_d, bt_d);
            const double w = right - left;
            const double h = top - bottom;
            if (h <= kMaxFilledRuleThicknessPt && w >= kMinLen && w > h) {
                const double yy = 0.5 * (bottom + top);
                page_data.rules.push_back({left, yy, right, yy, true, h});
            } else if (w <= kMaxFilledRuleThicknessPt && h >= kMinLen && h > w) {
                const double xx = 0.5 * (left + right);
                page_data.rules.push_back({xx, bottom, xx, top, false, w});
            }
            continue;
        }

        FS_MATRIX m{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        if (!FPDFPageObj_GetMatrix(obj, &m)) {
            m = {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        }

        float raw_stroke_w = 0.0F;
        double stroke_thickness = 0.0;
        if (FPDFPageObj_GetStrokeWidth(obj, &raw_stroke_w) && raw_stroke_w > 0.0F &&
            std::isfinite(raw_stroke_w)) {
            const double scale =
                0.5 * (std::hypot(static_cast<double>(m.a), static_cast<double>(m.b)) +
                       std::hypot(static_cast<double>(m.c), static_cast<double>(m.d)));
            stroke_thickness = static_cast<double>(raw_stroke_w) * (scale > 0.0 ? scale : 1.0);
        }

        // Collapse thin filled+stroked rectangular bars (`re B`) into one centerline.
        if (fillmode != FPDF_FILLMODE_NONE && nseg <= kMaxRectPathSegments) {
            float bl = 0.0F;
            float bb = 0.0F;
            float br = 0.0F;
            float bt = 0.0F;
            if (FPDFPageObj_GetBounds(obj, &bl, &bb, &br, &bt) && std::isfinite(bl) &&
                std::isfinite(bb) && std::isfinite(br) && std::isfinite(bt)) {
                const double bl_d = static_cast<double>(bl);
                const double br_d = static_cast<double>(br);
                const double bb_d = static_cast<double>(bb);
                const double bt_d = static_cast<double>(bt);
                const double left = (std::min)(bl_d, br_d);
                const double right = (std::max)(bl_d, br_d);
                const double bottom = (std::min)(bb_d, bt_d);
                const double top = (std::max)(bb_d, bt_d);
                const double w = right - left;
                const double h = top - bottom;
                if (h <= kThinStrokedBoxPt && w >= kMinLen && w > h) {
                    const double yy = 0.5 * (bottom + top);
                    page_data.rules.push_back({left, yy, right, yy, true, h + stroke_thickness});
                    continue;
                }
                if (w <= kThinStrokedBoxPt && h >= kMinLen && h > w) {
                    const double xx = 0.5 * (left + right);
                    page_data.rules.push_back({xx, bottom, xx, top, false, w + stroke_thickness});
                    continue;
                }
            }
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
                        {(std::min)(ax, bx), yy, (std::max)(ax, bx), yy, true, stroke_thickness});
                    return;
                }
                if (dx <= kEps && dy >= kMinLen) {
                    const double xx = (ax + bx) / 2.0;
                    page_data.rules.push_back(
                        {xx, (std::min)(ay, by), xx, (std::max)(ay, by), false, stroke_thickness});
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

bool quad_to_rect(const FS_QUADPOINTSF& qp, Rect& out) noexcept {
    if (!std::isfinite(qp.x1) || !std::isfinite(qp.y1) || !std::isfinite(qp.x2) ||
        !std::isfinite(qp.y2) || !std::isfinite(qp.x3) || !std::isfinite(qp.y3) ||
        !std::isfinite(qp.x4) || !std::isfinite(qp.y4)) {
        return false;
    }
    out.left = static_cast<double>(std::min({qp.x1, qp.x2, qp.x3, qp.x4}));
    out.right = static_cast<double>(std::max({qp.x1, qp.x2, qp.x3, qp.x4}));
    out.bottom = static_cast<double>(std::min({qp.y1, qp.y2, qp.y3, qp.y4}));
    out.top = static_cast<double>(std::max({qp.y1, qp.y2, qp.y3, qp.y4}));
    return out.right > out.left && out.top > out.bottom;
}

bool fs_rect_to_rect(const FS_RECTF& r, Rect& out) noexcept {
    if (!std::isfinite(r.left) || !std::isfinite(r.bottom) || !std::isfinite(r.right) ||
        !std::isfinite(r.top)) {
        return false;
    }
    const double rl = static_cast<double>(r.left);
    const double rr = static_cast<double>(r.right);
    const double rb = static_cast<double>(r.bottom);
    const double rt = static_cast<double>(r.top);
    const double left = (std::min)(rl, rr);
    const double right = (std::max)(rl, rr);
    const double bottom = (std::min)(rb, rt);
    const double top = (std::max)(rb, rt);
    out = Rect{left, bottom, right, top};
    return out.right > out.left && out.top > out.bottom;
}

constexpr double kExplicitLinkCoverRatio = 0.5;
constexpr double kLinkBaselineTolScale = 0.5;
constexpr double kUnderscoreBaselinePadPt = 0.5;

bool is_covered_by_explicit_link(const Rect& cand,
                                 std::span<const PageLink> explicit_links) noexcept {
    const double cx = 0.5 * (cand.left + cand.right);
    const double cy = 0.5 * (cand.bottom + cand.top);
    const double cand_area = (cand.right - cand.left) * (cand.top - cand.bottom);
    for (const PageLink& ex : explicit_links) {
        if (cx >= ex.left && cx <= ex.right && cy >= ex.bottom && cy <= ex.top) {
            return true;
        }
        const double ov_w = (std::min)(cand.right, ex.right) - (std::max)(cand.left, ex.left);
        const double ov_h = (std::min)(cand.top, ex.top) - (std::max)(cand.bottom, ex.bottom);
        if (ov_w > 0.0 && ov_h > 0.0 && cand_area > 0.0 &&
            ov_w * ov_h >= kExplicitLinkCoverRatio * cand_area) {
            return true;
        }
    }
    return false;
}

void extract_page_links(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_TEXTPAGE tp, PageData& page_data) {
    constexpr auto kMaxPageLinks =
        static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)());

    int start_pos = 0;
    FPDF_LINK link_annot = nullptr;
    while (page_data.links.size() < kMaxPageLinks &&
           FPDFLink_Enumerate(page, &start_pos, &link_annot)) {
        if (link_annot == nullptr) {
            continue;
        }
        FPDF_ACTION action = FPDFLink_GetAction(link_annot);
        if (action == nullptr || FPDFAction_GetType(action) != PDFACTION_URI) {
            continue;
        }
        const unsigned long uri_bytes = FPDFAction_GetURIPath(doc, action, nullptr, 0UL);
        if (uri_bytes <= kEmptyAsciiPayloadBytes) {
            continue;
        }
        std::string raw_uri(static_cast<std::size_t>(uri_bytes), '\0');
        if (FPDFAction_GetURIPath(doc, action, raw_uri.data(), uri_bytes) == 0UL) {
            continue;
        }
        const std::string uri = sanitize_link_uri(raw_uri);
        if (uri.empty()) {
            continue;
        }

        bool added_quad = false;
        const int quad_count = FPDFLink_CountQuadPoints(link_annot);
        for (int qi = 0; qi < quad_count && page_data.links.size() < kMaxPageLinks; ++qi) {
            FS_QUADPOINTSF qp{};
            Rect r{};
            if (FPDFLink_GetQuadPoints(link_annot, qi, &qp) && quad_to_rect(qp, r)) {
                page_data.links.push_back(PageLink{r.left, r.bottom, r.right, r.top, uri});
                added_quad = true;
            }
        }
        if (!added_quad && page_data.links.size() < kMaxPageLinks) {
            FS_RECTF annot_rect{};
            Rect r{};
            if (FPDFLink_GetAnnotRect(link_annot, &annot_rect) && fs_rect_to_rect(annot_rect, r)) {
                page_data.links.push_back(PageLink{r.left, r.bottom, r.right, r.top, uri});
            }
        }
    }

    const std::size_t explicit_count = page_data.links.size();

    // Implicit web links detected in prose; PageLinkHandle must close before tp.
    {
        PageLinkHandle web_links(FPDFLink_LoadWebLinks(tp));
        if (web_links) {
            const int web_count = FPDFLink_CountWebLinks(web_links.get());
            for (int wi = 0; wi < web_count && page_data.links.size() < kMaxPageLinks; ++wi) {
                const int units = FPDFLink_GetURL(web_links.get(), wi, nullptr, 0);
                if (units < kMinWebLinkCodeUnits) {
                    continue;
                }
                std::vector<unsigned short> u16_buf(static_cast<std::size_t>(units), 0U);
                const int got =
                    FPDFLink_GetURL(web_links.get(), wi, u16_buf.data(), units);
                if (got < kMinWebLinkCodeUnits) {
                    continue;
                }
                const std::string uri = sanitize_link_uri(utf16_units_to_utf8(
                    std::span<const unsigned short>(u16_buf.data(), static_cast<std::size_t>(got))));
                if (uri.empty()) {
                    continue;
                }
                const int rect_count = FPDFLink_CountRects(web_links.get(), wi);
                for (int ri = 0; ri < rect_count && page_data.links.size() < kMaxPageLinks; ++ri) {
                    double left = 0.0;
                    double top = 0.0;
                    double right = 0.0;
                    double bottom = 0.0;
                    if (!FPDFLink_GetRect(web_links.get(), wi, ri, &left, &top, &right, &bottom) ||
                        !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
                        !std::isfinite(bottom)) {
                        continue;
                    }
                    const double nl = (std::min)(left, right);
                    const double nr = (std::max)(left, right);
                    const double nb = (std::min)(bottom, top);
                    const double nt = (std::max)(bottom, top);
                    const Rect cand{nl, nb, nr, nt};
                    if (cand.right <= cand.left || cand.top <= cand.bottom) {
                        continue;
                    }
                    const std::span<const PageLink> explicit_span(page_data.links.data(),
                                                                  explicit_count);
                    if (!is_covered_by_explicit_link(cand, explicit_span)) {
                        page_data.links.push_back(
                            PageLink{cand.left, cand.bottom, cand.right, cand.top, uri});
                    }
                }
            }
        }
    }

    if (page_data.links.empty() || page_data.chars.empty()) {
        return;
    }

    for (CharInfo& c : page_data.chars) {
        if (c.space) {
            continue;
        }
        const double cx = 0.5 * (c.left + c.right);
        const double cy = 0.5 * (c.bottom + c.top);
        for (std::size_t li = 0U; li < page_data.links.size(); ++li) {
            const PageLink& lk = page_data.links[li];
            if (cx >= lk.left && cx <= lk.right && cy >= lk.bottom && cy <= lk.top) {
                c.link_id = static_cast<std::uint16_t>(li + 1U);
                break;
            }
        }
        // Low-ink '_' inside a link rect whose bottom sits on the baseline.
        if (c.link_id == 0U && c.code == U'_') {
            for (std::size_t li = 0U; li < page_data.links.size(); ++li) {
                const PageLink& lk = page_data.links[li];
                if (cx >= lk.left && cx <= lk.right &&
                    c.y >= lk.bottom - kUnderscoreBaselinePadPt && c.y <= lk.top) {
                    c.link_id = static_cast<std::uint16_t>(li + 1U);
                    break;
                }
            }
        }
    }

    // Propagate link_id across interior space runs between two glyphs sharing
    // the same nonzero link_id on the same baseline.
    std::vector<CharInfo>& chars = page_data.chars;
    std::size_t i = 1U;
    while (i + 1U < chars.size()) {
        if (!chars[i].space) {
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < chars.size() && chars[i].space) {
            ++i;
        }
        if (i >= chars.size()) {
            break;
        }
        const CharInfo& prev = chars[run_start - 1U];
        const CharInfo& next = chars[i];
        if (prev.link_id != 0U && prev.link_id == next.link_id && next.left >= prev.left) {
            const double tol = kLinkBaselineTolScale * std::max({prev.size, next.size, 1.0});
            if (std::fabs(prev.y - next.y) <= tol) {
                for (std::size_t k = run_start; k < i; ++k) {
                    chars[k].link_id = prev.link_id;
                }
            }
        }
    }
}

void extract_strikeout_annotations(FPDF_PAGE page, PageData& page_data) {
    if (page_data.chars.empty()) {
        return;
    }
    constexpr int kHiddenAnnotFlags =
        FPDF_ANNOT_FLAG_INVISIBLE | FPDF_ANNOT_FLAG_HIDDEN | FPDF_ANNOT_FLAG_NOVIEW;
    constexpr double kStrikeMinHorizOverlapRatio = 0.5;
    constexpr double kStrikeMinVertOverlapRatio = 0.25;

    std::vector<Rect> strike_rects;
    const int annot_count = FPDFPage_GetAnnotCount(page);
    for (int ai = 0; ai < annot_count; ++ai) {
        AnnotationHandle annot(FPDFPage_GetAnnot(page, ai));
        if (!annot || FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_STRIKEOUT) {
            continue;
        }
        if ((FPDFAnnot_GetFlags(annot.get()) & kHiddenAnnotFlags) != 0) {
            continue;
        }
        bool added_quad = false;
        const std::size_t quad_count = FPDFAnnot_CountAttachmentPoints(annot.get());
        for (std::size_t qi = 0U; qi < quad_count; ++qi) {
            FS_QUADPOINTSF qp{};
            Rect r{};
            if (FPDFAnnot_GetAttachmentPoints(annot.get(), qi, &qp) && quad_to_rect(qp, r)) {
                strike_rects.push_back(r);
                added_quad = true;
            }
        }
        if (!added_quad) {
            FS_RECTF rect{};
            Rect r{};
            if (FPDFAnnot_GetRect(annot.get(), &rect) && fs_rect_to_rect(rect, r)) {
                strike_rects.push_back(r);
            }
        }
    }

    if (strike_rects.empty()) {
        return;
    }

    for (CharInfo& c : page_data.chars) {
        if (c.space) {
            continue;
        }
        const double cx = 0.5 * (c.left + c.right);
        const double cy = 0.5 * (c.bottom + c.top);
        const double gw = c.right - c.left;
        const double gh = c.top - c.bottom;
        for (const Rect& sr : strike_rects) {
            if (cx >= sr.left && cx <= sr.right && cy >= sr.bottom && cy <= sr.top) {
                c.strikethrough = true;
                break;
            }
            const double ov_x = (std::min)(c.right, sr.right) - (std::max)(c.left, sr.left);
            const double ov_y = (std::min)(c.top, sr.top) - (std::max)(c.bottom, sr.bottom);
            const double min_h = (std::min)(gh, sr.top - sr.bottom);
            if (ov_x > 0.0 && ov_y > 0.0 &&
                (gw <= 0.0 || ov_x >= kStrikeMinHorizOverlapRatio * gw) &&
                (min_h <= 0.0 || ov_y >= kStrikeMinVertOverlapRatio * min_h)) {
                c.strikethrough = true;
                break;
            }
        }
    }

    std::vector<CharInfo>& chars = page_data.chars;
    std::size_t i = 1U;
    while (i + 1U < chars.size()) {
        if (!chars[i].space) {
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < chars.size() && chars[i].space) {
            ++i;
        }
        if (i >= chars.size()) {
            break;
        }
        const CharInfo& prev = chars[run_start - 1U];
        const CharInfo& next = chars[i];
        if (prev.strikethrough && next.strikethrough && next.left >= prev.left) {
            const double tol = kLinkBaselineTolScale * std::max({prev.size, next.size, 1.0});
            if (std::fabs(prev.y - next.y) <= tol) {
                for (std::size_t k = run_start; k < i; ++k) {
                    chars[k].strikethrough = true;
                }
            }
        }
    }
}

constexpr int kMaxOutlineDepth = 16;
constexpr std::size_t kMaxOutlineNodes = 4096U;
constexpr std::size_t kMaxDestViewParams = 4U;
constexpr std::size_t kFitHTopParamIndex = 0U;
constexpr std::size_t kFitRTopParamIndex = 3U;

OutlineEntry extract_outline_entry(FPDF_DOCUMENT doc, FPDF_BOOKMARK bm, int level) {
    OutlineEntry entry;
    entry.level = level;

    const unsigned long title_bytes = FPDFBookmark_GetTitle(bm, nullptr, 0UL);
    if (title_bytes > kEmptyUtf16PayloadBytes) {
        std::vector<unsigned char> buf(static_cast<std::size_t>(title_bytes));
        if (FPDFBookmark_GetTitle(bm, buf.data(), title_bytes) > 0UL) {
            entry.title =
                normalize_inline_separators(utf16le_to_utf8(buf.data(), buf.size()), true);
        }
    }

    FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm);
    if (dest == nullptr) {
        FPDF_ACTION act = FPDFBookmark_GetAction(bm);
        if (act != nullptr && FPDFAction_GetType(act) == PDFACTION_GOTO) {
            dest = FPDFAction_GetDest(doc, act);
        }
    }
    if (dest == nullptr) {
        return entry;
    }

    entry.page = FPDFDest_GetDestPageIndex(doc, dest);
    FPDF_BOOL has_x = 0;
    FPDF_BOOL has_y = 0;
    FPDF_BOOL has_zoom = 0;
    FS_FLOAT x = 0.0F;
    FS_FLOAT y = 0.0F;
    FS_FLOAT zoom = 0.0F;
    if (FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y, &zoom) &&
        has_y != 0 && std::isfinite(y)) {
        entry.y = static_cast<double>(y);
        entry.has_y = true;
        return entry;
    }

    unsigned long num_params = 0UL;
    std::array<FS_FLOAT, kMaxDestViewParams> params{};
    const unsigned long view = FPDFDest_GetView(dest, &num_params, params.data());
    if ((view == PDFDEST_VIEW_FITH || view == PDFDEST_VIEW_FITBH) &&
        num_params > kFitHTopParamIndex && std::isfinite(params[kFitHTopParamIndex])) {
        entry.y = static_cast<double>(params[kFitHTopParamIndex]);
        entry.has_y = true;
    } else if (view == PDFDEST_VIEW_FITR && num_params > kFitRTopParamIndex &&
               std::isfinite(params[kFitRTopParamIndex])) {
        entry.y = static_cast<double>(params[kFitRTopParamIndex]);
        entry.has_y = true;
    }
    return entry;
}

void collect_outlines_preorder(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, int level,
                               std::unordered_set<FPDF_BOOKMARK>& visited,
                               std::vector<OutlineEntry>& outlines) {
    if (level > kMaxOutlineDepth || visited.size() >= kMaxOutlineNodes) {
        return;
    }
    for (FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, parent); bm != nullptr;
         bm = FPDFBookmark_GetNextSibling(doc, bm)) {
        if (visited.size() >= kMaxOutlineNodes || !visited.insert(bm).second) {
            break;
        }
        OutlineEntry entry = extract_outline_entry(doc, bm, level);
        if (!entry.title.empty()) {
            outlines.push_back(std::move(entry));
        }
        if (level < kMaxOutlineDepth) {
            collect_outlines_preorder(doc, bm, level + 1, visited, outlines);
        }
    }
}

std::vector<OutlineEntry> extract_outlines(FPDF_DOCUMENT doc) {
    std::vector<OutlineEntry> outlines;
    std::unordered_set<FPDF_BOOKMARK> visited;
    collect_outlines_preorder(doc, nullptr, 1, visited, outlines);
    return outlines;
}

double loose_char_height(FPDF_TEXTPAGE tp, int index) {
    FS_RECTF rect{};
    if (!FPDFText_GetLooseCharBox(tp, index, &rect)) {
        return 0.0;
    }
    const double h = static_cast<double>(rect.top) - static_cast<double>(rect.bottom);
    return std::isfinite(h) ? h : 0.0;
}

constexpr char32_t kPdfiumDehyphenCtrl = 0x0002U;
constexpr char32_t kPdfiumDehyphenLegacy = 0xFFFEU;
constexpr char32_t kNonCharacterFfff = 0xFFFFU;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kMaxHorizontalAngleRad = 0.15;
constexpr double kDehyphenBaselineTolRatio = 0.7;
constexpr double kMinExpandedCharWidthPt = 0.01;
constexpr std::size_t kFontNameBufSize = 256U;

constexpr char32_t kSpacingDiaeresisCp = 0x00A8U;
constexpr char32_t kCombiningDiaeresisCp = 0x0308U;
constexpr char32_t kDotlessSmallICp = 0x0131U;
constexpr char32_t kSmallIWithDiaeresisCp = 0x00EFU;
constexpr double kAccentBaselineTolScale = 0.8;
constexpr double kAccentMaxGapScale = 0.6;
constexpr double kAccentMaxCenterDeltaScale = 0.8;

constexpr bool is_diaeresis_accent_cp(char32_t cp) noexcept {
    return cp == kSpacingDiaeresisCp || cp == kCombiningDiaeresisCp;
}

// Combines TeX overstruck spacing/combining diaeresis (U+00A8 / U+0308) and
// dotless 'i' (U+0131) into 'ï' (U+00EF) while preserving base glyph metrics.
void combine_tex_diaeresis_dotless_i(std::vector<CharInfo>& chars) {
    if (chars.size() < 2U) {
        return;
    }
    std::size_t write = 0U;
    for (std::size_t read = 0U; read < chars.size(); ++read) {
        if (read + 1U < chars.size()) {
            const CharInfo& a = chars[read];
            const CharInfo& b = chars[read + 1U];
            const bool a_accent_b_base = is_diaeresis_accent_cp(a.code) && b.code == kDotlessSmallICp;
            const bool a_base_b_accent = a.code == kDotlessSmallICp && is_diaeresis_accent_cp(b.code);
            if (a_accent_b_base || a_base_b_accent) {
                const double ref_sz = std::max({a.size, b.size, 1.0});
                const double overlap =
                    (std::min)(a.right, b.right) - (std::max)(a.left, b.left);
                const double center_delta =
                    std::fabs(0.5 * (a.left + a.right) - 0.5 * (b.left + b.right));
                if (std::fabs(a.y - b.y) <= kAccentBaselineTolScale * ref_sz &&
                    overlap >= -kAccentMaxGapScale * ref_sz &&
                    center_delta <= kAccentMaxCenterDeltaScale * ref_sz) {
                    CharInfo merged = a_accent_b_base ? b : a;
                    const CharInfo& accent = a_accent_b_base ? a : b;
                    merged.code = kSmallIWithDiaeresisCp;
                    merged.left = (std::min)(merged.left, accent.left);
                    merged.right = (std::max)(merged.right, accent.right);
                    merged.top = (std::max)(merged.top, accent.top);
                    chars[write++] = merged;
                    ++read;
                    continue;
                }
            }
        }
        if (write != read) {
            chars[write] = chars[read];
        }
        ++write;
    }
    chars.resize(write);
}

struct PageFontRecord {
    std::string name;
    int flags = 0;
    bool has_cjk = false;
    bool is_cjk_family = false;
};

std::size_t intern_page_font(std::vector<PageFontRecord>& fonts, std::string_view name, int flags) {
    for (std::size_t idx = 0U; idx < fonts.size(); ++idx) {
        if (fonts[idx].flags == flags && fonts[idx].name == name) {
            return idx;
        }
    }
    fonts.push_back(PageFontRecord{std::string(name), flags, false, is_cjk_font_name(name)});
    return fonts.size() - 1U;
}

void extract_page_chars(FPDF_TEXTPAGE tp, PageData& page_data, const ExtractOptions& options) {
    const int count = FPDFText_CountChars(tp);
    if (count <= 0) {
        return;
    }
    page_data.chars.reserve(static_cast<std::size_t>(count));

    std::vector<PageFontRecord> page_fonts;
    std::vector<std::size_t> char_font_ids;
    char_font_ids.reserve(static_cast<std::size_t>(count));

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
        if (cp >= kSurrogateHighStart && cp <= kSurrogateHighEnd) {
            const char32_t lo = i + 1 < count
                                    ? static_cast<char32_t>(FPDFText_GetUnicode(tp, i + 1))
                                    : 0U;
            if (lo < kSurrogateLowStart || lo > kSurrogateLowEnd) {
                continue;
            }
            cp = kFourByteMin +
                 ((cp - kSurrogateHighStart) << kSurrogateShift) +
                 (lo - kSurrogateLowStart);
            ++i;
        } else if (cp >= kSurrogateLowStart && cp <= kSurrogateLowEnd) {
            continue;
        }

        if (cp == 0U || cp == kNonCharacterFfff || is_line_break_cp(cp)) {
            continue;
        }

        const bool dehyphenated = cp == kPdfiumDehyphenCtrl || cp == kPdfiumDehyphenLegacy;
        if (dehyphenated) {
            cp = kSoftHyphenCp;
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

        const std::size_t font_id = intern_page_font(page_fonts, font_name, font_flags);
        if (is_cjk_cp(cp)) {
            page_fonts[font_id].has_cjk = true;
        }

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
        ch.mono = props.mono && !is_cjk_cp(cp) && !page_fonts[font_id].is_cjk_family;
        ch.space = is_space_cp(cp);
        if (ch.space) {
            cp = U' ';
        }

        ch.loose_height = loose_char_height(tp, i);

        if (debug_raw) {
            const int wt = FPDFText_GetFontWeight(tp, i);
            std::fprintf(
                stderr,
                "DBG U+%04X '%c' sz=%.2f loose=%.2f y=%.1f L=%.1f R=%.1f B=%.1f T=%.1f b=%d i=%d m=%d w=%d fl=%d font=%.*s\n",
                static_cast<unsigned>(cp),
                (cp >= kAsciiControlEnd && cp < kPrintableAsciiEnd) ? static_cast<char>(cp) : '?',
                ch.size, ch.loose_height, ch.y, ch.left, ch.right, ch.bottom, ch.top, props.bold,
                props.italic, ch.mono, wt, font_flags, static_cast<int>(font_name.size()),
                font_name.data());
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
                char_font_ids.push_back(font_id);
            }
            continue;
        }

        ch.code = cp;
        page_data.chars.push_back(ch);
        char_font_ids.push_back(font_id);
    }

    // Clear FixedPitch monospace flag on all characters drawn in any font that
    // emitted CJK glyphs on this page or matched a CJK font family name.
    for (std::size_t idx = 0U; idx < page_data.chars.size(); ++idx) {
        const PageFontRecord& fe = page_fonts[char_font_ids[idx]];
        if (fe.has_cjk || fe.is_cjk_family) {
            page_data.chars[idx].mono = false;
        }
    }

    combine_tex_diaeresis_dotless_i(page_data.chars);
    repair_ocr_text_layer(page_data);
}

}  // namespace

static Extraction run_extract_pdf(const std::filesystem::path& file,
                                  const ExtractOptions& options) {
    // Perform file I/O outside the process-wide PDFium mutex.
    const std::vector<char> data = read_file(file);

    Extraction result;
    std::vector<PendingPageImage> pending_images;
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
        result.meta.subject = get_meta_text(doc.get(), "Subject");
        result.meta.keywords = get_meta_text(doc.get(), "Keywords");
        result.meta.creator = get_meta_text(doc.get(), "Creator");
        result.meta.producer = get_meta_text(doc.get(), "Producer");
        result.meta.creation_date = normalize_pdf_date(get_meta_text(doc.get(), "CreationDate"));
        result.meta.mod_date = normalize_pdf_date(get_meta_text(doc.get(), "ModDate"));
        result.meta.outlines = extract_outlines(doc.get());

        const std::size_t expected_pages =
            options.pages.empty()
                ? static_cast<std::size_t>((std::max)(result.meta.page_count, 0))
                : options.pages.size();
        result.pages.reserve(expected_pages);

        std::unordered_set<std::uint64_t> captured_hashes;

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
                    if (options.extract_links) {
                        extract_page_links(doc.get(), page.get(), tp.get(), page_data);
                    }
                }
            }

            extract_strikeout_annotations(page.get(), page_data);

            const std::size_t page_vec_index = result.pages.size();
            std::vector<Rect> suppressed_scans;
            if (options.extract_images) {
                collect_page_raster_images(doc.get(), page.get(), page_data, options,
                                           page_vec_index, captured_hashes, pending_images,
                                           suppressed_scans);
                if (!options.image_dir.empty()) {
                    for (const Rect& scan : suppressed_scans) {
                        collect_scan_diagram_bands(page.get(), page_data, page_vec_index, scan,
                                                   pending_images);
                    }
                }
            }

            extract_page_rules(page.get(), page_data);
            static const bool debug_rules = has_env_flag("PDF2MD_DEBUG_RULES");
            if (debug_rules) {
                for (const RuleSegment& r : page_data.rules) {
                    std::fprintf(stderr, "RULE p%d %s x0=%.1f y0=%.1f x1=%.1f y1=%.1f w=%.2f\n",
                                 p + 1, r.horizontal ? "H" : "V", r.x0, r.y0, r.x1, r.y1,
                                 r.thickness);
                }
            }

            const std::vector<FigureRegion> figures = detect_figure_regions(
                page.get(), page_data.width, page_data.height, page_data.chars);
            if (!figures.empty()) {
                erase_figure_text_labels(figures, page_data.chars);
                if (options.extract_images && !options.image_dir.empty()) {
                    collect_vector_figure_renders(page.get(), page_data, figures, page_vec_index,
                                                  pending_images);
                }
            }

            result.pages.push_back(std::move(page_data));
        }
    }

    // Perform CPU-heavy PNG deflate encoding and filesystem writes outside PdfiumInit::mutex().
    if (!pending_images.empty()) {
        flush_pending_images(pending_images, result.pages, options);
    }

    repair_inflated_font_sizes(result.pages);
    return result;
}

}  // namespace pdf2md::pdf

namespace pdf2md {

struct PdfExtractor::Impl {
    ExtractOptions options;

    explicit Impl(ExtractOptions opts) : options(std::move(opts)) {}
};

PdfExtractor::PdfExtractor(ExtractOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PdfExtractor::PdfExtractor(const PdfExtractor& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

PdfExtractor& PdfExtractor::operator=(const PdfExtractor& other) {
    if (this == &other) {
        return *this;
    }
    if (!other.impl_) {
        impl_.reset();
        return *this;
    }
    auto copy = std::make_unique<Impl>(*other.impl_);
    impl_ = std::move(copy);
    return *this;
}

PdfExtractor::PdfExtractor(PdfExtractor&& other) noexcept = default;

PdfExtractor& PdfExtractor::operator=(PdfExtractor&& other) noexcept = default;

PdfExtractor::~PdfExtractor() = default;

Extraction PdfExtractor::extract(const std::filesystem::path& file) const {
    if (!impl_) {
        const ExtractOptions default_options{};
        return pdf::run_extract_pdf(file, default_options);
    }
    return pdf::run_extract_pdf(file, impl_->options);
}

Extraction extract_pdf(const std::filesystem::path& file, const ExtractOptions& options) {
    return PdfExtractor(options).extract(file);
}

}  // namespace pdf2md
