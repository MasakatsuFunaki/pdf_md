#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace pdf2md {

enum class PixelFormat : std::uint8_t {
    Gray = 1,
    BGR = 2,
    BGRx = 3,
    BGRA = 4,
};

int bytes_per_pixel(PixelFormat format) noexcept;

// Non-owning view over a 2-D uncompressed bitmap buffer.
struct BitmapView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::BGRA;
};

// Owned 2-D pixel buffer decoupled from PDFium C handles so PNG encoding and
// disk I/O can run outside PdfiumInit::mutex() and unit tests need no PDFium.
struct RasterImage {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::BGRA;

    RasterImage();
    RasterImage(int w, int h, PixelFormat fmt, int row_stride = 0);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] BitmapView view() const noexcept;
};

// Writes a bitmap view as a PNG file. Returns false on failure.
bool write_bitmap_png(const BitmapView& bitmap, const std::filesystem::path& out_path);

// FNV-1a hash of active bitmap pixel channels, used to deduplicate repeated images.
std::uint64_t hash_bitmap(const BitmapView& bitmap);

// Fraction of pixels darker than a near-white threshold -- how much "ink" the
// bitmap carries. Used to tell a scanned diagram crop from blank paper.
double bitmap_ink_fraction(const BitmapView& bitmap);

// Writes a scanned-band crop: first trims away any text-line-height run of ink
// hugging the top or bottom edge that a clear white gap separates from the rest
// (the neighbouring text line bleeding into the band -- the OCR geometry the
// band was cut by sits a few points off the print), then crops to the ink
// bounding box plus `pad` pixels. Returns false (writing nothing) when what
// remains is blank or under `min_ink_fraction` -- blank paper is not a figure.
bool write_scan_band_png(const BitmapView& bitmap, const std::filesystem::path& out_path,
                         int pad, double min_ink_fraction);

}  // namespace pdf2md
