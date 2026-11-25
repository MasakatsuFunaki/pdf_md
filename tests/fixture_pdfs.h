#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Synthetic PDF generators, RAII filesystem sandboxes, and real PNG consumer
// verification helpers for the component test suite.
namespace pdf2md::testfx {

// Thread-compatible RAII temporary directory under std::filesystem::temp_directory_path()
// that removes its entire directory tree when leaving scope.
class ScopedSandbox {
public:
    explicit ScopedSandbox(std::string_view tag);

    ScopedSandbox(const ScopedSandbox&) = delete;
    ScopedSandbox& operator=(const ScopedSandbox&) = delete;
    ScopedSandbox(ScopedSandbox&&) = delete;
    ScopedSandbox& operator=(ScopedSandbox&&) = delete;

    ~ScopedSandbox() noexcept;

    const std::filesystem::path& path() const noexcept { return dir_; }
    std::filesystem::path file(std::string_view relative) const;

private:
    std::filesystem::path dir_;
};

struct PngWireReport {
    bool valid = false;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    std::size_t idat_bytes = 0;
};

struct PngConsumerReport {
    bool valid = false;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bits_per_component = 0;
};

// Reads the exact binary contents of `path`. Throws std::runtime_error if `path`
// cannot be opened for reading.
std::vector<std::uint8_t> read_binary_bytes(const std::filesystem::path& path);

// Writes `bytes` verbatim to `path` (creating parent directories if needed).
void write_binary_bytes(const std::filesystem::path& path,
                        const std::vector<std::uint8_t>& bytes);

// Extracts every target path from Markdown `![alt](target)` image embeds in order.
std::vector<std::string> extract_markdown_image_paths(std::string_view markdown);

// Validates PNG signature, IHDR dimensions/depth/color-type, IDAT presence,
// IEND termination, and IEEE 802.3 CRC-32 checksums on every chunk.
PngWireReport inspect_png_wire(const std::filesystem::path& png_path);

// Decodes `png_path` through libharu's libpng+zlib reader (`HPDF_LoadPngImageFromMem`).
// When `roundtrip_pdf_out` is non-empty, embeds the decoded image into a fresh
// single-page PDF so `pdf2md::Converter` (PDFium) can consume it back.
PngConsumerReport consume_png_with_libharu(
    const std::filesystem::path& png_path,
    const std::filesystem::path& roundtrip_pdf_out = {});

// Three pages: title, headings, styled runs, lists, code block, hyphenated
// lines, a cross-page paragraph, and repeated headers/footers on every page.
void make_basic(const std::filesystem::path& out);

// One page with a full-width title above two columns of prose.
void make_twocol(const std::filesystem::path& out);

// One page with a title, a 3-column table and a trailing paragraph.
void make_table(const std::filesystem::path& out);

// One page with a paragraph and an embedded raw RGB image.
void make_image(const std::filesystem::path& out);

// Nine pages exercising position-anchored running-head stripping: pages 1-4
// carry a chapter-scoped running head at a fixed top-margin position (too few
// pages for the document-wide repeat threshold), pages 5-8 a borderless table
// whose header row repeats at a fixed top-margin position with data rows right
// beneath it (must survive), page 9 is plain. Every page has unique body text.
void make_runheads(const std::filesystem::path& out);

// Generates all canonical fixtures into `dir` (created if needed) and returns it.
std::filesystem::path generate_all(const std::filesystem::path& dir);

// Synthetic vector-diagram form XObject fixture: when `diagram` is true the form
// is placed at sub-unity scale (0.5x) so interior labels are suppressed and the
// figure is rasterized; when false it is placed at unit scale (in-page form).
void make_vector_diagram(const std::filesystem::path& out, bool diagram,
                         bool overhang = false, bool label_row = false);

// Five-page scanned-book fixture with full-page background scans, render-mode-3
// 1pt scaled OCR text, misplaced space glyph, tight word gap, repeated margin
// watermark, embedded sub-page figure, cover page, and dark scan diagram band.
void make_scanned_ocr(const std::filesystem::path& out);

// One-page PDF with `/ToUnicode`-mapped 2/3/4-byte UTF-8 code points (Latin-1,
// ligatures U+FB01/U+FB02, Greek, CJK, astral surrogate pair U+1D11E) plus a
// 45-degree rotated watermark for `keep_rotated` verification.
void make_unicode_and_rotated(const std::filesystem::path& out);

// Two-page PDF with stroked vector grid borders forming a 3-column ruled table
// at the bottom of page 1 that continues with identical column edges at the top
// of page 2, followed by closing prose.
void make_multipage_ruled_table(const std::filesystem::path& out);

// Two-page PDF embedding a shared RGB image on both pages plus a distinct second
// image on page 1, exercising content-hash PNG deduplication and ordering.
void make_multi_image_dedup(const std::filesystem::path& out);

// Single-page password-encrypted PDF (RC4-128 / R3) requiring `user_password`.
void make_encrypted(const std::filesystem::path& out,
                    const std::string& owner_password,
                    const std::string& user_password);

}  // namespace pdf2md::testfx
