#include "fixture_pdfs.h"

#include <hpdf.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pdf2md::testfx {

namespace {

constexpr int kMaxSandboxAttempts = 32;
constexpr std::size_t kStreamReadChunkSize = 4096;
constexpr std::size_t kPngSignatureBytes = 8;
constexpr std::size_t kPngChunkHeaderBytes = 8;
constexpr std::size_t kPngCrcBytes = 4;
constexpr std::size_t kPngIhdrPayloadBytes = 13;
constexpr std::size_t kMinValidPngBytes =
    kPngSignatureBytes + kPngChunkHeaderBytes + kPngIhdrPayloadBytes + kPngCrcBytes +
    kPngChunkHeaderBytes + 1 + kPngCrcBytes + kPngChunkHeaderBytes + kPngCrcBytes;
constexpr std::uint8_t kExpectedPngBitDepth = 8;
constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320U;
constexpr std::uint32_t kCrc32Initial = 0xFFFFFFFFU;
constexpr std::array<std::uint8_t, kPngSignatureBytes> kPngSignature = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

std::atomic<std::uint64_t> g_sandbox_counter{0};

std::string to_hex_u64(std::uint64_t value) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    constexpr int kNibbles = 16;
    std::string out(kNibbles, '0');
    for (int i = kNibbles - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHexDigits[value & 0x0FU];
        value >>= 4U;
    }
    return out;
}

std::uint32_t read_be_u32(const std::vector<std::uint8_t>& buf, std::size_t offset) {
    return (static_cast<std::uint32_t>(buf[offset]) << 24U) |
           (static_cast<std::uint32_t>(buf[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(buf[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(buf[offset + 3]);
}

std::uint32_t compute_crc32(const std::uint8_t* data, std::size_t length) noexcept {
    std::uint32_t crc = kCrc32Initial;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = (crc & 1U) != 0U ? kCrc32Polynomial : 0U;
            crc = (crc >> 1U) ^ mask;
        }
    }
    return ~crc;
}

bool is_valid_png_color_type(std::uint8_t color_type) noexcept {
    return color_type == 0U || color_type == 2U || color_type == 3U ||
           color_type == 4U || color_type == 6U;
}

void error_handler(HPDF_STATUS error_no, HPDF_STATUS detail_no, void*) {
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "libharu error 0x%04X detail %u",
                  static_cast<unsigned>(error_no), static_cast<unsigned>(detail_no));
    throw std::runtime_error(buf);
}

struct Fonts {
    HPDF_Font regular = nullptr;
    HPDF_Font bold = nullptr;
    HPDF_Font italic = nullptr;
    HPDF_Font mono = nullptr;
};

Fonts load_fonts(HPDF_Doc pdf) {
    return Fonts{
        HPDF_GetFont(pdf, "Helvetica", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Helvetica-Bold", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Helvetica-Oblique", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Courier", "WinAnsiEncoding"),
    };
}

// RAII owner so a throwing libharu error handler never leaks `HPDF_Doc`.
class Doc {
public:
    Doc() : pdf_(HPDF_New(error_handler, nullptr)) {
        if (!pdf_) {
            throw std::runtime_error("HPDF_New failed");
        }
    }

    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&&) = delete;
    Doc& operator=(Doc&&) = delete;

    ~Doc() noexcept {
        if (pdf_) {
            HPDF_Free(pdf_);
        }
    }

    HPDF_Doc get() const noexcept { return pdf_; }

private:
    HPDF_Doc pdf_ = nullptr;
};

// Saves `pdf` via an in-memory stream and `std::filesystem::path`-aware binary
// output so Unicode paths on Windows work identically to POSIX.
void save_pdf_to_path(HPDF_Doc pdf, const std::filesystem::path& out) {
    HPDF_SaveToStream(pdf);
    HPDF_ResetStream(pdf);
    const HPDF_UINT32 total_size = HPDF_GetStreamSize(pdf);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(total_size);

    std::array<HPDF_BYTE, kStreamReadChunkSize> chunk{};
    while (bytes.size() < total_size) {
        auto read_len = static_cast<HPDF_UINT32>(chunk.size());
        const HPDF_STATUS st = HPDF_ReadFromStream(pdf, chunk.data(), &read_len);
        if (read_len == 0) {
            break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read_len);
        if (st == HPDF_STREAM_EOF) {
            break;
        }
    }
    write_binary_bytes(out, bytes);
}

void write_raw_pdf_objects(const std::filesystem::path& out,
                           const std::vector<std::string>& objs) {
    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets(objs.size() + 1, 0);
    for (std::size_t i = 0; i < objs.size(); ++i) {
        offsets[i + 1] = pdf.size();
        pdf += objs[i];
    }
    const std::size_t xref_offset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (std::size_t i = 1; i <= objs.size(); ++i) {
        char entry[24]{};
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[i]);
        pdf += entry;
    }
    pdf += "trailer<</Size " + std::to_string(objs.size() + 1) +
           "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref_offset) + "\n%%EOF";

    std::vector<std::uint8_t> raw;
    raw.reserve(pdf.size());
    for (char ch : pdf) {
        raw.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
    }
    write_binary_bytes(out, raw);
}

std::string make_obj_stream(int obj_num, const std::string& dict, const std::string& body) {
    return std::to_string(obj_num) + " 0 obj<<" + dict + "/Length " +
           std::to_string(body.size()) + ">>stream\n" + body + "\nendstream endobj\n";
}

void text_at(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char* text) {
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, font, size);
    HPDF_Page_TextOut(page, x, y, text);
    HPDF_Page_EndText(page);
}

float text_width(HPDF_Page page, HPDF_Font font, float size, const char* text) {
    HPDF_Page_SetFontAndSize(page, font, size);
    return HPDF_Page_TextWidth(page, text);
}

void draw_line(HPDF_Page page, float x0, float y0, float x1, float y1) {
    HPDF_Page_SetLineWidth(page, 0.75f);
    HPDF_Page_MoveTo(page, x0, y0);
    HPDF_Page_LineTo(page, x1, y1);
    HPDF_Page_Stroke(page);
}

HPDF_Page add_letter_page(HPDF_Doc pdf) {
    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
    return page;
}

void header_footer(HPDF_Page page, const Fonts& fonts, int page_no) {
    text_at(page, fonts.regular, 9, 72, 758, "PDF2MD Fixture Suite");
    const std::string footer = "Page " + std::to_string(page_no);
    text_at(page, fonts.regular, 9, 290, 36, footer.c_str());
}

}  // namespace

ScopedSandbox::ScopedSandbox(std::string_view tag) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    std::random_device rd;
    for (int attempt = 0; attempt < kMaxSandboxAttempts; ++attempt) {
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t rnd =
            (static_cast<std::uint64_t>(rd()) << 32U) ^ static_cast<std::uint64_t>(rd());
        const std::uint64_t seq = ++g_sandbox_counter;
        std::filesystem::path candidate =
            base / ("pdf2md_ct_" + std::string(tag) + "_" + to_hex_u64(tick ^ rnd) + "_" +
                    std::to_string(seq));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec) && !ec) {
            dir_ = std::move(candidate);
            return;
        }
    }
    throw std::runtime_error("Unable to allocate unique ScopedSandbox directory");
}

ScopedSandbox::~ScopedSandbox() noexcept {
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
}

std::filesystem::path ScopedSandbox::file(std::string_view relative) const {
    return dir_ / std::filesystem::path(relative);
}

std::vector<std::uint8_t> read_binary_bytes(const std::filesystem::path& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        throw std::runtime_error("Cannot open binary file for reading: " + path.string());
    }
    std::vector<std::uint8_t> out;
    for (std::istreambuf_iterator<char> it(is), end; it != end; ++it) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(*it)));
    }
    return out;
}

void write_binary_bytes(const std::filesystem::path& path,
                        const std::vector<std::uint8_t>& bytes) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) {
        throw std::runtime_error("Cannot open binary file for writing: " + path.string());
    }
    for (std::uint8_t b : bytes) {
        os.put(static_cast<char>(b));
    }
    if (!os) {
        throw std::runtime_error("Failed writing binary file: " + path.string());
    }
}

std::vector<std::string> extract_markdown_image_paths(std::string_view markdown) {
    std::vector<std::string> paths;
    std::size_t pos = 0;
    while (pos < markdown.size()) {
        const std::size_t bang = markdown.find("![", pos);
        if (bang == std::string_view::npos) {
            break;
        }
        const std::size_t mid = markdown.find("](", bang + 2);
        if (mid == std::string_view::npos) {
            break;
        }
        const std::string_view alt = markdown.substr(bang + 2, mid - (bang + 2));
        if (alt.find('\n') != std::string_view::npos) {
            pos = bang + 2;
            continue;
        }
        if (mid + 2 < markdown.size() && markdown[mid + 2] == '<') {
            const std::size_t close_angle = markdown.find(">)", mid + 3);
            if (close_angle == std::string_view::npos) {
                break;
            }
            const std::string_view wrapped = markdown.substr(mid + 3, close_angle - (mid + 3));
            if (wrapped.empty() || wrapped.find('\n') != std::string_view::npos) {
                pos = mid + 3;
                continue;
            }
            paths.emplace_back(wrapped);
            pos = close_angle + 2;
            continue;
        }
        const std::size_t close = markdown.find(')', mid + 2);
        if (close == std::string_view::npos) {
            break;
        }
        std::string_view target = markdown.substr(mid + 2, close - (mid + 2));
        if (target.size() >= 2U && target.front() == '<' && target.back() == '>') {
            target = target.substr(1, target.size() - 2);
        }
        if (target.empty() || target.find('\n') != std::string_view::npos) {
            pos = mid + 2;
            continue;
        }
        paths.emplace_back(target);
        pos = close + 1;
    }
    return paths;
}

PngWireReport inspect_png_wire(const std::filesystem::path& png_path) {
    PngWireReport report;
    std::error_code ec;
    if (!std::filesystem::exists(png_path, ec)) {
        report.error = "PNG file does not exist";
        return report;
    }
    const std::vector<std::uint8_t> bytes = read_binary_bytes(png_path);
    if (bytes.size() < kMinValidPngBytes) {
        report.error = "PNG file smaller than minimum valid chunk sequence";
        return report;
    }
    for (std::size_t i = 0; i < kPngSignatureBytes; ++i) {
        if (bytes[i] != kPngSignature[i]) {
            report.error = "Invalid 8-byte PNG signature";
            return report;
        }
    }

    std::size_t offset = kPngSignatureBytes;
    bool seen_ihdr = false;
    bool seen_iend = false;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kPngChunkHeaderBytes + kPngCrcBytes) {
            report.error = "Truncated PNG chunk header/CRC";
            return report;
        }
        const std::uint32_t length = read_be_u32(bytes, offset);
        if (bytes.size() - offset - kPngChunkHeaderBytes - kPngCrcBytes < length) {
            report.error = "PNG chunk length exceeds remaining file size";
            return report;
        }
        const std::string type(
            {static_cast<char>(bytes[offset + 4]), static_cast<char>(bytes[offset + 5]),
             static_cast<char>(bytes[offset + 6]), static_cast<char>(bytes[offset + 7])});
        const std::uint32_t expected_crc =
            read_be_u32(bytes, offset + kPngChunkHeaderBytes + length);
        const std::uint32_t actual_crc =
            compute_crc32(&bytes[offset + 4], static_cast<std::size_t>(length) + 4U);
        if (actual_crc != expected_crc) {
            report.error = "CRC-32 mismatch in chunk " + type;
            return report;
        }

        const std::size_t data_pos = offset + kPngChunkHeaderBytes;
        if (!seen_ihdr) {
            if (type != "IHDR" || length != kPngIhdrPayloadBytes) {
                report.error = "First PNG chunk must be a 13-byte IHDR";
                return report;
            }
            report.width = read_be_u32(bytes, data_pos);
            report.height = read_be_u32(bytes, data_pos + 4);
            report.bit_depth = bytes[data_pos + 8];
            report.color_type = bytes[data_pos + 9];
            const std::uint8_t compression = bytes[data_pos + 10];
            const std::uint8_t filter = bytes[data_pos + 11];
            const std::uint8_t interlace = bytes[data_pos + 12];
            if (report.width == 0U || report.height == 0U) {
                report.error = "IHDR carries zero width or height";
                return report;
            }
            if (report.bit_depth != kExpectedPngBitDepth ||
                !is_valid_png_color_type(report.color_type) || compression != 0U ||
                filter != 0U || interlace > 1U) {
                report.error = "IHDR metadata outside valid 8-bit PNG specification";
                return report;
            }
            seen_ihdr = true;
        } else if (type == "IDAT") {
            if (seen_iend) {
                report.error = "IDAT chunk appeared after IEND";
                return report;
            }
            report.idat_bytes += length;
        } else if (type == "IEND") {
            if (length != 0U) {
                report.error = "IEND chunk must have zero payload length";
                return report;
            }
            seen_iend = true;
            offset += kPngChunkHeaderBytes + length + kPngCrcBytes;
            if (offset != bytes.size()) {
                report.error = "Trailing bytes after PNG IEND chunk";
                return report;
            }
            break;
        }

        offset += kPngChunkHeaderBytes + length + kPngCrcBytes;
    }

    if (!seen_ihdr || !seen_iend || report.idat_bytes == 0U) {
        report.error = "PNG missing required IHDR, non-empty IDAT, or IEND chunk";
        return report;
    }
    report.valid = true;
    return report;
}

PngConsumerReport consume_png_with_libharu(
    const std::filesystem::path& png_path,
    const std::filesystem::path& roundtrip_pdf_out) {
    PngConsumerReport report;
    try {
        const std::vector<std::uint8_t> bytes = read_binary_bytes(png_path);
        if (bytes.empty()) {
            report.error = "PNG file is empty";
            return report;
        }
        Doc doc;
        HPDF_Doc pdf = doc.get();
        HPDF_Image img = HPDF_LoadPngImageFromMem(
            pdf, bytes.data(), static_cast<HPDF_UINT>(bytes.size()));
        if (!img) {
            report.error = "HPDF_LoadPngImageFromMem returned null";
            return report;
        }
        report.width = HPDF_Image_GetWidth(img);
        report.height = HPDF_Image_GetHeight(img);
        report.bits_per_component = HPDF_Image_GetBitsPerComponent(img);
        if (report.width == 0U || report.height == 0U) {
            report.error = "Decoded libharu image has zero dimensions";
            return report;
        }

        if (!roundtrip_pdf_out.empty()) {
            const Fonts fonts = load_fonts(pdf);
            HPDF_Page page = add_letter_page(pdf);
            text_at(page, fonts.regular, 11, 72, 700, "Roundtrip consumer caption above image.");
            HPDF_Page_DrawImage(page, img, 72, 540, 96, 96);
            save_pdf_to_path(pdf, roundtrip_pdf_out);
        }
        report.valid = true;
    } catch (const std::exception& ex) {
        report.error = ex.what();
    }
    return report;
}

void make_basic(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Basic Fixture Document");
    HPDF_SetInfoAttr(pdf, HPDF_INFO_AUTHOR, "pdf2md tests");
    const Fonts fonts = load_fonts(pdf);

    HPDF_Page p1 = add_letter_page(pdf);
    header_footer(p1, fonts, 1);

    text_at(p1, fonts.bold, 24, 72, 700, "Sample Document Title");
    text_at(p1, fonts.bold, 16, 72, 660, "First Section");

    float y = 634;
    text_at(p1, fonts.regular, 11, 72, y,
            "This is the first paragraph of the sample document. It spans multiple");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y,
            "lines and should be joined into a single markdown paragraph without");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "unwanted line breaks.");

    y -= 30;
    float x = 72;
    const char* seg1 = "The converter must detect ";
    text_at(p1, fonts.regular, 11, x, y, seg1);
    x += text_width(p1, fonts.regular, 11, seg1);
    const char* seg2 = "bold text";
    text_at(p1, fonts.bold, 11, x, y, seg2);
    x += text_width(p1, fonts.bold, 11, seg2);
    const char* seg3 = " and also ";
    text_at(p1, fonts.regular, 11, x, y, seg3);
    x += text_width(p1, fonts.regular, 11, seg3);
    const char* seg4 = "italic text";
    text_at(p1, fonts.italic, 11, x, y, seg4);
    x += text_width(p1, fonts.italic, 11, seg4);
    text_at(p1, fonts.regular, 11, x, y, " inside a sentence.");

    text_at(p1, fonts.bold, 16, 72, y - 40, "Lists");
    y -= 64;
    text_at(p1, fonts.regular, 11, 72, y, "\225 First bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "\225 Second bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 100, y, "\225 Nested bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "\225 Third bullet item");
    y -= 22;
    text_at(p1, fonts.regular, 11, 72, y, "1. Alpha step");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "2. Beta step");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "3. Gamma step");

    text_at(p1, fonts.bold, 16, 72, y - 40, "Code Example");
    y -= 64;
    text_at(p1, fonts.mono, 10, 72, y, "int main() {");
    y -= 13;
    text_at(p1, fonts.mono, 10, 72, y, "    return 42;");
    y -= 13;
    text_at(p1, fonts.mono, 10, 72, y, "}");

    HPDF_Page p2 = add_letter_page(pdf);
    header_footer(p2, fonts, 2);
    text_at(p2, fonts.bold, 16, 72, 700, "Hyphenation");
    text_at(p2, fonts.regular, 11, 72, 674, "This paragraph exercises the hyphen-");
    text_at(p2, fonts.regular, 11, 72, 659,
            "ation repair logic across wrapped lines of the same para-");
    text_at(p2, fonts.regular, 11, 72, 644, "graph in the converter.");

    text_at(p2, fonts.bold, 16, 72, 600, "Bold Pseudo Heading Section");
    text_at(p2, fonts.regular, 11, 72, 574,
            "A paragraph that continues onto the next page must be merged into a");
    text_at(p2, fonts.regular, 11, 72, 559,
            "single paragraph even though it is split across the page");

    HPDF_Page p3 = add_letter_page(pdf);
    header_footer(p3, fonts, 3);
    text_at(p3, fonts.regular, 11, 72, 700,
            "boundary, which is exactly what this final sentence verifies.");
    text_at(p3, fonts.regular, 11, 72, 660,
            "Final page content for the header stripping threshold.");

    save_pdf_to_path(pdf, out);
}

void make_twocol(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Two Column Fixture");
    const Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.bold, 18, 72, 720, "Two Column Layout Test");

    constexpr std::array<const char*, 4> kLeftColumn = {
        "Left column sentence one flows",
        "across several short lines and",
        "must be read completely before",
        "any right column text appears.",
    };
    constexpr std::array<const char*, 4> kRightColumn = {
        "Right column sentence one also",
        "flows across several lines and",
        "must appear after the whole of",
        "the left column in the output.",
    };
    float y = 680;
    for (const char* line : kLeftColumn) {
        text_at(page, fonts.regular, 11, 72, y, line);
        y -= 15;
    }
    y = 680;
    for (const char* line : kRightColumn) {
        text_at(page, fonts.regular, 11, 340, y, line);
        y -= 15;
    }

    save_pdf_to_path(pdf, out);
}

void make_table(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Table Fixture");
    const Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.bold, 18, 72, 720, "Inventory Report");

    constexpr std::array<float, 3> kColX = {72.0f, 250.0f, 420.0f};
    constexpr const char* kRows[4][3] = {
        {"Name", "Quantity", "Price"},
        {"Apples", "12", "1.50"},
        {"Bananas", "7", "0.75"},
        {"Cherries", "31", "4.20"},
    };
    float y = 680;
    for (std::size_t r = 0; r < 4; ++r) {
        const HPDF_Font font = (r == 0) ? fonts.bold : fonts.regular;
        for (std::size_t c = 0; c < 3; ++c) {
            text_at(page, font, 11, kColX[c], y, kRows[r][c]);
        }
        y -= 16;
    }

    text_at(page, fonts.regular, 11, 72, y - 24,
            "A regular paragraph after the table should not be absorbed into it.");

    save_pdf_to_path(pdf, out);
}

void make_image(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Image Fixture");
    const Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.regular, 11, 72, 700, "A paragraph above the picture.");

    constexpr int kImgDim = 8;
    constexpr int kRgbChannels = 3;
    constexpr std::array<HPDF_BYTE, 4> kEdgeByteSeam = {0x00U, 0x7FU, 0x80U, 0xFFU};
    std::array<HPDF_BYTE, kImgDim * kImgDim * kRgbChannels> pixels{};
    for (int yy = 0; yy < kImgDim; ++yy) {
        for (int xx = 0; xx < kImgDim; ++xx) {
            const auto cell = static_cast<std::size_t>(xx / 2 + yy / 2);
            const auto base = static_cast<std::size_t>((yy * kImgDim + xx) * kRgbChannels);
            pixels[base] = kEdgeByteSeam[cell % kEdgeByteSeam.size()];
            pixels[base + 1] = kEdgeByteSeam[(cell + 2U) % kEdgeByteSeam.size()];
            pixels[base + 2] =
                kEdgeByteSeam[static_cast<std::size_t>(xx + yy) % kEdgeByteSeam.size()];
        }
    }
    HPDF_Image img = HPDF_LoadRawImageFromMem(
        pdf, pixels.data(), kImgDim, kImgDim, HPDF_CS_DEVICE_RGB, kExpectedPngBitDepth);
    HPDF_Page_DrawImage(page, img, 72, 560, 96, 96);

    text_at(page, fonts.regular, 11, 72, 520, "A paragraph below the picture.");

    save_pdf_to_path(pdf, out);
}

void make_runheads(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Running Heads Fixture");
    const Fonts fonts = load_fonts(pdf);

    constexpr std::array<const char*, 9> kBodies = {
        "Opening paragraph about alpha topics on the first page.",
        "Second page discusses beta material in its own words.",
        "Third page continues with gamma related observations.",
        "Fourth page rounds out the delta considerations nicely.",
        "Fifth page presents epsilon data in tabular form below.",
        "Sixth page presents zeta data in tabular form below.",
        "Seventh page presents eta data in tabular form below.",
        "Eighth page presents theta data in tabular form below.",
        "Ninth page closes the fixture with plain iota prose.",
    };
    constexpr const char* kCountries[4][3] = {
        {"France", "Paris", "68"},
        {"Japan", "Tokyo", "125"},
        {"Brazil", "Brasilia", "216"},
        {"Kenya", "Nairobi", "55"},
    };
    for (std::size_t p = 0; p < kBodies.size(); ++p) {
        HPDF_Page page = add_letter_page(pdf);
        if (p < 4) {
            text_at(page, fonts.regular, 9, 72, 758, "GAMMA CHAPTER 7");
        } else if (p < 8) {
            text_at(page, fonts.bold, 9, 72, 758, "Country");
            text_at(page, fonts.bold, 9, 200, 758, "Capital");
            text_at(page, fonts.bold, 9, 330, 758, "Population");
            text_at(page, fonts.regular, 9, 72, 744, kCountries[p - 4][0]);
            text_at(page, fonts.regular, 9, 200, 744, kCountries[p - 4][1]);
            text_at(page, fonts.regular, 9, 330, 744, kCountries[p - 4][2]);
        }
        text_at(page, fonts.regular, 11, 72, 400, kBodies[p]);
    }
    save_pdf_to_path(pdf, out);
}

std::filesystem::path generate_all(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    make_basic(dir / "basic.pdf");
    make_twocol(dir / "twocol.pdf");
    make_table(dir / "table.pdf");
    make_image(dir / "image.pdf");
    make_runheads(dir / "runheads.pdf");
    return dir;
}

void make_vector_diagram(const std::filesystem::path& out, bool diagram, bool overhang,
                         bool label_row) {
    std::string form = "0 0 0 rg\n";
    constexpr int kNestedRectCount = 16;
    for (int i = 0; i < kNestedRectCount; ++i) {
        const int x = 20 + (i % 4) * 140;
        const int y = 40 + (i / 4) * 140;
        form += std::to_string(x) + " " + std::to_string(y) + " 90 90 re f\n";
    }
    form += "BT /F1 40 Tf 120 300 Td (SECRETLABELWORD) Tj ET\n";

    const std::string scale = diagram ? "0.5 0 0 0.5" : "1 0 0 1";
    std::string page;
    page += "BT /F1 12 Tf 150 360 Td (DIAGRAMCAPTIONWORD) Tj ET\n";
    page += "q " + scale + " 150 400 cm /Fm1 Do Q\n";
    if (overhang) {
        page += "BT /F1 10 Tf 155 660 Td "
                "(This overhanging body sentence must remain visible in output) Tj ET\n";
        page += "BT /F1 10 Tf 250 520 Td (STRAYLABEL) Tj ET\n";
    }
    if (label_row) {
        page += "BT /F1 10 Tf 160 630 Td (LEFTLABELCLUSTERWORD) Tj ET\n";
        page += "BT /F1 10 Tf 330 630 Td (RIGHTLABELCLUSTERWD) Tj ET\n";
    }

    const std::vector<std::string> objs = {
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n",
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n",
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 4 0 R"
        "/Resources<</Font<</F1 6 0 R>>/XObject<</Fm1 5 0 R>>>>>>endobj\n",
        make_obj_stream(4, "", page),
        make_obj_stream(5,
                        "/Type/XObject/Subtype/Form/BBox[0 0 600 600]"
                        "/Resources<</Font<</F1 6 0 R>>>>",
                        form),
        "6 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n",
    };
    write_raw_pdf_objects(out, objs);
}

void make_scanned_ocr(const std::filesystem::path& out) {
    const auto ocr_line = [](double size, double x, double y, const std::string& text) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%.1f 0 0 %.1f %.1f %.1f Tm ", size, size, x, y);
        return std::string(buf) + "(" + text + ") Tj\n";
    };
    const auto page_content = [&](const std::string& body_lines, bool with_figure) {
        std::string s = "q 612 0 0 792 0 0 cm /Im1 Do Q\n";
        if (with_figure) {
            s += "q 96 0 0 64 400 150 cm /Im2 Do Q\n";
        }
        s += "BT 3 Tr /F1 1 Tf\n" + body_lines + "ET\n";
        s += "BT /F1 9 Tf 1 0 0 1 560 400 Tm (draftmark) Tj ET\n";
        return s;
    };

    const std::string p1 = page_content(
        ocr_line(11, 72, 640,
                 "The scanned optical layer must survive with every word intact") +
            ocr_line(11, 72, 626, "and join into one readable paragraph of body prose.") +
            ocr_line(11, 72, 612, "misplaced") + ocr_line(11, 112, 612, " ") +
            ocr_line(11, 124, 612, "spaces") + ocr_line(11, 72, 598, "alpha") +
            ocr_line(11, 100.9, 598, "beta"),
        false);
    const std::string p2 = page_content(
        ocr_line(11, 72, 640, "Second page copy keeps its own sentences readable too.") +
            ocr_line(11, 72, 560, "A caption may accompany the small genuine figure below."),
        true);
    const std::string p3 = page_content(
        ocr_line(11, 72, 640, "Third page text exists so repetition thresholds are met."),
        false);
    const std::string p4 = page_content(ocr_line(11, 72, 640, "Cover"), false);

    std::string p5 = "q 612 0 0 792 0 0 cm /Im3 Do Q\n";
    p5 += "BT 3 Tr /F1 1 Tf\n" +
          ocr_line(11, 72, 700, "Fifth page prose sits above the scanned diagram region.") +
          ocr_line(11, 72, 686, "More prose follows directly beneath the first sentence.") +
          ocr_line(11, 72, 150, "Closing sentence beneath the diagram wraps the page.") + "ET\n";
    p5 += "BT /F1 9 Tf 1 0 0 1 560 400 Tm (draftmark) Tj ET\n";

    constexpr std::size_t kScanPixelCount = 16;
    const std::string scan_pixels(kScanPixelCount, '\xDD');
    std::string figure_pixels;
    figure_pixels.reserve(kScanPixelCount * 3);
    for (int i = 0; i < static_cast<int>(kScanPixelCount); ++i) {
        const bool on = ((i / 4 + i % 4) % 2) != 0;
        figure_pixels += static_cast<char>(on ? 0xE0 : 0x20);
        figure_pixels += static_cast<char>(on ? 0x20 : 0xE0);
        figure_pixels += '\x80';
    }
    std::string diagram_scan_pixels(kScanPixelCount, '\xDD');
    for (std::size_t i = 4; i < 12; ++i) {
        diagram_scan_pixels[i] = '\x30';
    }
    const std::string page_dict =
        "/Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 10 0 R>>"
        "/XObject<</Im1 11 0 R/Im2 12 0 R/Im3 16 0 R>>>>";
    const std::vector<std::string> objs = {
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n",
        "2 0 obj<</Type/Pages/Kids[3 0 R 4 0 R 5 0 R 6 0 R 14 0 R]/Count 5>>endobj\n",
        "3 0 obj<<" + page_dict + "/Contents 7 0 R>>endobj\n",
        "4 0 obj<<" + page_dict + "/Contents 8 0 R>>endobj\n",
        "5 0 obj<<" + page_dict + "/Contents 9 0 R>>endobj\n",
        "6 0 obj<<" + page_dict + "/Contents 13 0 R>>endobj\n",
        make_obj_stream(7, "", p1),
        make_obj_stream(8, "", p2),
        make_obj_stream(9, "", p3),
        "10 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n",
        make_obj_stream(11,
                        "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceGray"
                        "/BitsPerComponent 8",
                        scan_pixels),
        make_obj_stream(12,
                        "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceRGB"
                        "/BitsPerComponent 8",
                        figure_pixels),
        make_obj_stream(13, "", p4),
        "14 0 obj<<" + page_dict + "/Contents 15 0 R>>endobj\n",
        make_obj_stream(15, "", p5),
        make_obj_stream(16,
                        "/Type/XObject/Subtype/Image/Width 4/Height 4/ColorSpace/DeviceGray"
                        "/BitsPerComponent 8",
                        diagram_scan_pixels),
    };
    write_raw_pdf_objects(out, objs);
}

void make_unicode_and_rotated(const std::filesystem::path& out) {
    // Maps 'A'..'J' (0x41..0x4A) in proportional Helvetica to multi-byte Unicode:
    //   A -> U+00E9 (e-acute)        F -> U+65E5 (CJK 'day')
    //   B -> U+00DF (sharp s)        G -> U+672C (CJK 'origin')
    //   C -> U+FB01 ('fi' ligature)  H -> U+8A9E (CJK 'language')
    //   D -> U+FB02 ('fl' ligature)  I -> U+1D11E (UTF-16 surrogate pair D834 DD1E)
    //   E -> U+00A9 (copyright, 0x80+ UTF-8 boundary)  J -> U+20AC (Euro sign)
    const std::string cmap =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<00> <FF>\n"
        "endcodespacerange\n"
        "10 beginbfchar\n"
        "<41> <00E9>\n"
        "<42> <00DF>\n"
        "<43> <FB01>\n"
        "<44> <FB02>\n"
        "<45> <00A9>\n"
        "<46> <65E5>\n"
        "<47> <672C>\n"
        "<48> <8A9E>\n"
        "<49> <D834DD1E>\n"
        "<4A> <20AC>\n"
        "endbfchar\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n";

    std::string page;
    page += "BT /F1 18 Tf 72 720 Td (Unicode Seam Verification) Tj ET\n";
    page += "BT /F3 11 Tf 72 680 Td (Caf) Tj /F2 11 Tf <41> Tj "
            "/F3 11 Tf ( Stra) Tj /F2 11 Tf <42> Tj "
            "/F3 11 Tf (e pro) Tj /F2 11 Tf <43> Tj "
            "/F3 11 Tf (le work) Tj /F2 11 Tf <44> Tj "
            "/F3 11 Tf (ow ) Tj /F2 11 Tf <454A> Tj "
            "/F3 11 Tf ( ) Tj /F2 11 Tf <464748> Tj "
            "/F3 11 Tf ( clef ) Tj /F2 11 Tf <49> Tj ET\n";
    page += "BT /F3 16 Tf 0.9781 0.2079 -0.2079 0.9781 220 420 Tm "
            "(ZX9) Tj ET\n";

    const std::vector<std::string> objs = {
        "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n",
        "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n",
        "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 4 0 R"
        "/Resources<</Font<</F1 5 0 R/F2 6 0 R/F3 8 0 R>>>>>>endobj\n",
        make_obj_stream(4, "", page),
        "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica-Bold>>endobj\n",
        "6 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica"
        "/FirstChar 65/LastChar 74"
        "/Widths[667 667 722 722 667 611 778 722 278 500]"
        "/ToUnicode 7 0 R>>endobj\n",
        make_obj_stream(7, "", cmap),
        "8 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n",
    };
    write_raw_pdf_objects(out, objs);
}

void make_multipage_ruled_table(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Ruled Multi-Page Table");
    const Fonts fonts = load_fonts(pdf);

    constexpr std::array<float, 4> kColEdges = {72.0f, 220.0f, 370.0f, 520.0f};
    constexpr std::array<float, 3> kTextX = {80.0f, 228.0f, 378.0f};

    // Page 1: title, intro prose, and a 3-row bordered grid near the bottom margin.
    HPDF_Page p1 = add_letter_page(pdf);
    text_at(p1, fonts.bold, 18, 72, 720, "Quarterly Revenue Ledger");
    text_at(p1, fonts.regular, 11, 72, 685,
            "Opening ledger prose precedes the multi-page bordered table.");

    constexpr std::array<float, 4> kPage1RowY = {220.0f, 196.0f, 172.0f, 148.0f};
    for (float y : kPage1RowY) {
        draw_line(p1, kColEdges.front(), y, kColEdges.back(), y);
    }
    for (float x : kColEdges) {
        draw_line(p1, x, kPage1RowY.back(), x, kPage1RowY.front());
    }
    constexpr const char* kPage1Cells[3][3] = {
        {"Region", "Quarter", "Revenue"},
        {"North", "Q1", "120.5"},
        {"South", "Q2", "98.0"},
    };
    for (std::size_t r = 0; r < 3; ++r) {
        const HPDF_Font font = (r == 0) ? fonts.bold : fonts.regular;
        const float baseline = kPage1RowY[r] - 16.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            text_at(p1, font, 10, kTextX[c], baseline, kPage1Cells[r][c]);
        }
    }

    // Page 2: continuation grid with identical vertical column boundaries at the
    // top of the page, followed by a closing paragraph.
    HPDF_Page p2 = add_letter_page(pdf);
    constexpr std::array<float, 3> kPage2RowY = {720.0f, 696.0f, 672.0f};
    for (float y : kPage2RowY) {
        draw_line(p2, kColEdges.front(), y, kColEdges.back(), y);
    }
    for (float x : kColEdges) {
        draw_line(p2, x, kPage2RowY.back(), x, kPage2RowY.front());
    }
    constexpr const char* kPage2Cells[2][3] = {
        {"East", "Q3", "145.2"},
        {"West", "Q4", "163.8"},
    };
    for (std::size_t r = 0; r < 2; ++r) {
        const float baseline = kPage2RowY[r] - 16.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            text_at(p2, fonts.regular, 10, kTextX[c], baseline, kPage2Cells[r][c]);
        }
    }
    text_at(p2, fonts.regular, 11, 72, 620,
            "Ledger summary concludes after all four regional rows.");

    save_pdf_to_path(pdf, out);
}

void make_multi_image_dedup(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Image Deduplication Fixture");
    const Fonts fonts = load_fonts(pdf);

    constexpr int kBadgeDim = 8;
    constexpr int kChartW = 12;
    constexpr int kChartH = 8;
    constexpr int kRgbChannels = 3;

    constexpr std::array<HPDF_BYTE, 4> kEdgeByteSeam = {0x00U, 0x7FU, 0x80U, 0xFFU};
    std::array<HPDF_BYTE, kBadgeDim * kBadgeDim * kRgbChannels> badge_pixels{};
    for (int yy = 0; yy < kBadgeDim; ++yy) {
        for (int xx = 0; xx < kBadgeDim; ++xx) {
            const auto seam = static_cast<std::size_t>(xx + yy);
            const auto idx = static_cast<std::size_t>((yy * kBadgeDim + xx) * kRgbChannels);
            badge_pixels[idx] = kEdgeByteSeam[seam % kEdgeByteSeam.size()];
            badge_pixels[idx + 1] = kEdgeByteSeam[(seam + 1U) % kEdgeByteSeam.size()];
            badge_pixels[idx + 2] = kEdgeByteSeam[(seam + 3U) % kEdgeByteSeam.size()];
        }
    }
    std::array<HPDF_BYTE, kChartW * kChartH * kRgbChannels> chart_pixels{};
    for (int yy = 0; yy < kChartH; ++yy) {
        for (int xx = 0; xx < kChartW; ++xx) {
            const auto idx = static_cast<std::size_t>((yy * kChartW + xx) * kRgbChannels);
            chart_pixels[idx] = kEdgeByteSeam[static_cast<std::size_t>(xx) % kEdgeByteSeam.size()];
            chart_pixels[idx + 1] = static_cast<HPDF_BYTE>(40 + yy * 24);
            chart_pixels[idx + 2] =
                kEdgeByteSeam[static_cast<std::size_t>(yy) % kEdgeByteSeam.size()];
        }
    }

    HPDF_Image badge = HPDF_LoadRawImageFromMem(
        pdf, badge_pixels.data(), kBadgeDim, kBadgeDim, HPDF_CS_DEVICE_RGB,
        kExpectedPngBitDepth);
    HPDF_Image chart = HPDF_LoadRawImageFromMem(
        pdf, chart_pixels.data(), kChartW, kChartH, HPDF_CS_DEVICE_RGB, kExpectedPngBitDepth);

    HPDF_Page p1 = add_letter_page(pdf);
    text_at(p1, fonts.regular, 11, 72, 710, "First page introduces shared badge and chart.");
    HPDF_Page_DrawImage(p1, badge, 72, 580, 80, 80);
    HPDF_Page_DrawImage(p1, chart, 72, 450, 120, 80);

    HPDF_Page p2 = add_letter_page(pdf);
    text_at(p2, fonts.regular, 11, 72, 710, "Second page repeats the shared badge asset.");
    HPDF_Page_DrawImage(p2, badge, 72, 580, 80, 80);

    save_pdf_to_path(pdf, out);
}

void make_encrypted(const std::filesystem::path& out,
                    const std::string& owner_password,
                    const std::string& user_password) {
    Doc doc;
    HPDF_Doc pdf = doc.get();
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Encrypted Fixture");
    const Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.bold, 18, 72, 720, "Confidential Briefing");
    text_at(page, fonts.regular, 11, 72, 680,
            "Authorized eyes only payload verified through password decryption.");

    constexpr HPDF_UINT kRc4KeyLengthBytes = 16;
    HPDF_SetPassword(pdf, owner_password.c_str(), user_password.c_str());
    HPDF_SetEncryptionMode(pdf, HPDF_ENCRYPT_R3, kRc4KeyLengthBytes);

    save_pdf_to_path(pdf, out);
}

}  // namespace pdf2md::testfx
