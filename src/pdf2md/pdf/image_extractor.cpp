#include "pdf2md/image_extractor.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace pdf2md {

int bytes_per_pixel(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::Gray:
            return 1;
        case PixelFormat::BGR:
            return 3;
        case PixelFormat::BGRx:
        case PixelFormat::BGRA:
            return 4;
    }
    return 0;
}

RasterImage::RasterImage() = default;

RasterImage::RasterImage(int w, int h, PixelFormat fmt, int row_stride) {
    const int bpp = bytes_per_pixel(fmt);
    if (w <= 0 || h <= 0 || bpp <= 0) {
        return;
    }
    const auto min_row = static_cast<std::int64_t>(w) * static_cast<std::int64_t>(bpp);
    if (min_row > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
        return;
    }
    const int actual_stride = row_stride == 0 ? static_cast<int>(min_row) : row_stride;
    if (static_cast<std::int64_t>(actual_stride) < min_row) {
        return;
    }
    const auto max_bytes = (std::numeric_limits<std::size_t>::max)();
    const auto row_sz = static_cast<std::size_t>(actual_stride);
    if (static_cast<std::size_t>(h) > max_bytes / row_sz) {
        return;
    }
    pixels.assign(static_cast<std::size_t>(h) * row_sz, 0U);
    width = w;
    height = h;
    stride = actual_stride;
    format = fmt;
}

bool RasterImage::empty() const noexcept {
    return pixels.empty() || width <= 0 || height <= 0 || stride <= 0;
}

BitmapView RasterImage::view() const noexcept {
    if (empty()) {
        return {};
    }
    return BitmapView{pixels.data(), width, height, stride, format};
}

}  // namespace pdf2md

namespace pdf2md::pdf {

namespace {

constexpr int kGrayChannels = 1;
constexpr int kRgbChannels = 3;
constexpr int kRgbaChannels = 4;

constexpr int kInkLuminanceThreshold = 200;
constexpr int kMinInkyPixelsPerRow = 3;
constexpr int kEdgeTrimSearchRows = 34;
constexpr int kMaxBledTextRunRows = 28;
constexpr int kMinWhiteSeparatorRows = 12;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct ValidatedBitmap {
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::BGRA;
    int source_bpp = 0;
    int packed_channels = 0;
    const unsigned char* buffer = nullptr;
    bool valid = false;
};

ValidatedBitmap inspect_bitmap(const BitmapView& bitmap) noexcept {
    ValidatedBitmap view;
    if (bitmap.data == nullptr || bitmap.width <= 0 || bitmap.height <= 0 || bitmap.stride <= 0) {
        return view;
    }
    view.width = bitmap.width;
    view.height = bitmap.height;
    view.stride = bitmap.stride;
    view.format = bitmap.format;
    view.buffer = bitmap.data;

    switch (view.format) {
        case PixelFormat::Gray:
            view.source_bpp = kGrayChannels;
            view.packed_channels = kGrayChannels;
            break;
        case PixelFormat::BGR:
            view.source_bpp = kRgbChannels;
            view.packed_channels = kRgbChannels;
            break;
        case PixelFormat::BGRx:
            view.source_bpp = kRgbaChannels;
            view.packed_channels = kRgbChannels;
            break;
        case PixelFormat::BGRA:
            view.source_bpp = kRgbaChannels;
            view.packed_channels = kRgbaChannels;
            break;
        default:
            return view;
    }

    const auto min_row_bytes =
        static_cast<std::int64_t>(view.width) * static_cast<std::int64_t>(view.source_bpp);
    if (static_cast<std::int64_t>(view.stride) < min_row_bytes) {
        return view;
    }

    const auto max_bytes = (std::numeric_limits<std::size_t>::max)();
    const auto row_stride = static_cast<std::size_t>(view.stride);
    if (static_cast<std::size_t>(view.height) > max_bytes / row_stride) {
        return view;
    }
    const auto row_packed =
        static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.packed_channels);
    if (row_packed == 0U || static_cast<std::size_t>(view.height) > max_bytes / row_packed) {
        return view;
    }

    view.valid = true;
    return view;
}

int pixel_luminance(const ValidatedBitmap& view, const unsigned char* px) noexcept {
    if (view.source_bpp == kGrayChannels) {
        return px[0];
    }
    return (static_cast<int>(px[0]) + static_cast<int>(px[1]) + static_cast<int>(px[2])) /
           kRgbChannels;
}

// Packs the sub-rectangle [x0..x0+cw) x [y0..y0+ch) into tightly packed Gray, RGB, or RGBA rows.
void pack_subrect(const ValidatedBitmap& view, int x0, int y0, int cw, int ch,
                  std::vector<unsigned char>& out) {
    const auto dst_row_bytes =
        static_cast<std::size_t>(cw) * static_cast<std::size_t>(view.packed_channels);
    out.resize(static_cast<std::size_t>(ch) * dst_row_bytes);

    for (int ry = 0; ry < ch; ++ry) {
        const unsigned char* src_row =
            view.buffer + static_cast<std::size_t>(y0 + ry) * static_cast<std::size_t>(view.stride) +
            static_cast<std::size_t>(x0) * static_cast<std::size_t>(view.source_bpp);
        unsigned char* dst_row = out.data() + static_cast<std::size_t>(ry) * dst_row_bytes;

        switch (view.format) {
            case PixelFormat::Gray:
                std::copy_n(src_row, static_cast<std::size_t>(cw), dst_row);
                break;
            case PixelFormat::BGR:
                for (int x = 0; x < cw; ++x) {
                    dst_row[3 * x + 0] = src_row[3 * x + 2];
                    dst_row[3 * x + 1] = src_row[3 * x + 1];
                    dst_row[3 * x + 2] = src_row[3 * x + 0];
                }
                break;
            case PixelFormat::BGRx:
                for (int x = 0; x < cw; ++x) {
                    dst_row[3 * x + 0] = src_row[4 * x + 2];
                    dst_row[3 * x + 1] = src_row[4 * x + 1];
                    dst_row[3 * x + 2] = src_row[4 * x + 0];
                }
                break;
            case PixelFormat::BGRA:
                for (int x = 0; x < cw; ++x) {
                    dst_row[4 * x + 0] = src_row[4 * x + 2];
                    dst_row[4 * x + 1] = src_row[4 * x + 1];
                    dst_row[4 * x + 2] = src_row[4 * x + 0];
                    dst_row[4 * x + 3] = src_row[4 * x + 3];
                }
                break;
        }
    }
}

void stbi_write_to_ofstream(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) {
        return;
    }
    auto* os = static_cast<std::ofstream*>(context);
    os->write(static_cast<const char*>(data), size);
}

bool write_packed_png(const std::filesystem::path& out_path, int w, int h, int channels,
                      const unsigned char* data, int stride) {
    if (out_path.empty() || w <= 0 || h <= 0 || data == nullptr) {
        return false;
    }

    std::error_code ec;
    if (std::filesystem::is_directory(out_path, ec)) {
        return false;
    }

    // Create the target directory on demand, so a conversion that turns out to
    // hold no images leaves no empty output directory behind.
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }

    std::ofstream os(out_path, std::ios::binary);
    if (!os) {
        return false;
    }
    const int ok =
        stbi_write_png_to_func(stbi_write_to_ofstream, &os, w, h, channels, data, stride);
    os.close();
    if (ok == 0 || !os.good()) {
        std::filesystem::remove(out_path, ec);
        return false;
    }
    return true;
}

void fnv_mix_byte(std::uint64_t& hash, unsigned char byte) noexcept {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnvPrime;
}

void fnv_mix_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    fnv_mix_byte(hash, static_cast<unsigned char>(value & 0xFFU));
    fnv_mix_byte(hash, static_cast<unsigned char>((value >> 8U) & 0xFFU));
    fnv_mix_byte(hash, static_cast<unsigned char>((value >> 16U) & 0xFFU));
    fnv_mix_byte(hash, static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

}  // namespace

}  // namespace pdf2md::pdf

namespace pdf2md {

bool write_bitmap_png(const BitmapView& bitmap, const std::filesystem::path& out_path) {
    const pdf::ValidatedBitmap view = pdf::inspect_bitmap(bitmap);
    if (!view.valid) {
        return false;
    }

    std::vector<unsigned char> pixels;
    pdf::pack_subrect(view, 0, 0, view.width, view.height, pixels);
    return pdf::write_packed_png(out_path, view.width, view.height, view.packed_channels,
                                 pixels.data(), view.width * view.packed_channels);
}

bool write_scan_band_png(const BitmapView& bitmap, const std::filesystem::path& out_path,
                         int pad, double min_ink_fraction) {
    const pdf::ValidatedBitmap view = pdf::inspect_bitmap(bitmap);
    if (!view.valid || !std::isfinite(min_ink_fraction)) {
        return false;
    }

    const int w = view.width;
    const int h = view.height;

    // Per-row ink counts at the render's 2x scale (so a text line is ~26px).
    std::vector<int> row_dark(static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row =
            view.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(view.stride);
        int count = 0;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px =
                row + static_cast<std::size_t>(x) * static_cast<std::size_t>(view.source_bpp);
            if (pdf::pixel_luminance(view, px) < pdf::kInkLuminanceThreshold) {
                ++count;
            }
        }
        row_dark[static_cast<std::size_t>(y)] = count;
    }

    const auto inky = [&](int y) noexcept {
        return row_dark[static_cast<std::size_t>(y)] >= pdf::kMinInkyPixelsPerRow;
    };

    // Trim a text-height inky run hugging an edge when a clear white gap
    // separates it from the rest -- a bled neighbour text line.
    const auto trim = [&](int from, int dir) noexcept {
        int r = from;
        while (r != from + dir * pdf::kEdgeTrimSearchRows && r >= 0 && r < h && !inky(r)) {
            r += dir;
        }
        if (r < 0 || r >= h || !inky(r) || std::abs(r - from) >= pdf::kEdgeTrimSearchRows) {
            return from;
        }
        int run_end = r;
        while (run_end + dir >= 0 && run_end + dir < h && inky(run_end + dir)) {
            run_end += dir;
        }
        if (std::abs(run_end - r) + 1 > pdf::kMaxBledTextRunRows) {
            return from;
        }
        for (int g = 1; g <= pdf::kMinWhiteSeparatorRows; ++g) {
            const int y = run_end + dir * g;
            if (y < 0 || y >= h || inky(y)) {
                return from;
            }
        }
        return run_end + dir * (pdf::kMinWhiteSeparatorRows / 2);
    };

    const int top = trim(0, +1);
    const int bottom = trim(h - 1, -1);
    if (bottom <= top) {
        return false;
    }

    std::size_t dark = 0;
    int x0 = w;
    int y0 = h;
    int x1 = -1;
    int y1 = -1;
    for (int y = top; y <= bottom; ++y) {
        const unsigned char* row =
            view.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(view.stride);
        for (int x = 0; x < w; ++x) {
            const unsigned char* px =
                row + static_cast<std::size_t>(x) * static_cast<std::size_t>(view.source_bpp);
            if (pdf::pixel_luminance(view, px) >= pdf::kInkLuminanceThreshold) {
                continue;
            }
            ++dark;
            x0 = (std::min)(x0, x);
            y0 = (std::min)(y0, y);
            x1 = (std::max)(x1, x);
            y1 = (std::max)(y1, y);
        }
    }
    if (x1 < 0) {
        return false;
    }
    const double total_pixels = static_cast<double>(w) * static_cast<double>(h);
    if (static_cast<double>(dark) < min_ink_fraction * total_pixels) {
        return false;
    }

    const int safe_pad = (std::max)(0, pad);
    x0 = (std::max)(0, x0 - safe_pad);
    y0 = (std::max)(top, y0 - safe_pad);
    x1 = (std::min)(w - 1, x1 + safe_pad);
    y1 = (std::min)(bottom, y1 + safe_pad);
    const int cw = x1 - x0 + 1;
    const int ch = y1 - y0 + 1;

    std::vector<unsigned char> cropped;
    pdf::pack_subrect(view, x0, y0, cw, ch, cropped);
    return pdf::write_packed_png(out_path, cw, ch, view.packed_channels, cropped.data(),
                                 cw * view.packed_channels);
}

double bitmap_ink_fraction(const BitmapView& bitmap) {
    const pdf::ValidatedBitmap view = pdf::inspect_bitmap(bitmap);
    if (!view.valid) {
        return 0.0;
    }

    std::size_t dark = 0;
    for (int y = 0; y < view.height; ++y) {
        const unsigned char* row =
            view.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(view.stride);
        for (int x = 0; x < view.width; ++x) {
            const unsigned char* px =
                row + static_cast<std::size_t>(x) * static_cast<std::size_t>(view.source_bpp);
            if (pdf::pixel_luminance(view, px) < pdf::kInkLuminanceThreshold) {
                ++dark;
            }
        }
    }

    const double count = static_cast<double>(view.width) * static_cast<double>(view.height);
    return static_cast<double>(dark) / count;
}

std::uint64_t hash_bitmap(const BitmapView& bitmap) {
    const pdf::ValidatedBitmap view = pdf::inspect_bitmap(bitmap);
    if (!view.valid) {
        return 0U;
    }

    std::uint64_t hash = pdf::kFnvOffsetBasis;
    pdf::fnv_mix_u32(hash, static_cast<std::uint32_t>(view.width));
    pdf::fnv_mix_u32(hash, static_cast<std::uint32_t>(view.height));
    pdf::fnv_mix_u32(hash, static_cast<std::uint32_t>(view.format));

    for (int y = 0; y < view.height; ++y) {
        const unsigned char* row =
            view.buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(view.stride);
        if (view.format == PixelFormat::BGRx) {
            // Hash only the active B, G, R channels, skipping both the unused x
            // padding byte and any row-end stride padding.
            for (int x = 0; x < view.width; ++x) {
                const unsigned char* px = row + static_cast<std::size_t>(x) * 4U;
                pdf::fnv_mix_byte(hash, px[0]);
                pdf::fnv_mix_byte(hash, px[1]);
                pdf::fnv_mix_byte(hash, px[2]);
            }
            continue;
        }
        const std::size_t active_row_bytes =
            static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.packed_channels);
        for (std::size_t i = 0; i < active_row_bytes; ++i) {
            pdf::fnv_mix_byte(hash, row[i]);
        }
    }
    return hash;
}

}  // namespace pdf2md
