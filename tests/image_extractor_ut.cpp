#include "image_extractor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

void ensure_pdfium_initialized() {
    static const int init_once = [] {
        FPDF_InitLibrary();
        return 1;
    }();
    (void)init_once;
}

struct BitmapDeleter {
    void operator()(FPDF_BITMAP bmp) const noexcept {
        if (bmp != nullptr) {
            FPDFBitmap_Destroy(bmp);
        }
    }
};

using ScopedBitmap = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, BitmapDeleter>;

ScopedBitmap make_bitmap(int width, int height, int format, void* external_buf = nullptr,
                         int stride = 0) {
    ensure_pdfium_initialized();
    return ScopedBitmap(FPDFBitmap_CreateEx(width, height, format, external_buf, stride));
}

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<uint64_t> counter{0};
        std::random_device rd;
        const auto ticks = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        const std::string dir_name = "pdf2md_img_ut_" + std::to_string(ticks) + "_" +
                                     std::to_string(rd()) + "_" + std::to_string(id);
        path_ = fs::temp_directory_path() / dir_name;
        fs::create_directories(path_);
    }

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// RFC 2083 PNG wire format header and trailer layout.
constexpr std::array<uint8_t, 8> kPngSignature = {
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
};
constexpr std::array<uint8_t, 4> kIhdrTag = {0x49U, 0x48U, 0x44U, 0x52U};
constexpr std::array<uint8_t, 12> kIendChunk = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4EU, 0x44U, 0xAEU, 0x42U, 0x60U, 0x82U,
};
constexpr uint32_t kIhdrPayloadLength = 13U;
constexpr std::size_t kMinValidPngBytes = 8U + 25U + 12U;
constexpr std::size_t kIhdrLengthOffset = 8U;
constexpr std::size_t kIhdrTagOffset = 12U;
constexpr std::size_t kIhdrWidthOffset = 16U;
constexpr std::size_t kIhdrHeightOffset = 20U;
constexpr std::size_t kIhdrBitDepthOffset = 24U;
constexpr std::size_t kIhdrColorTypeOffset = 25U;
constexpr uint8_t kBitsPerByte = 8U;

uint32_t read_be_u32(const std::vector<uint8_t>& buf, std::size_t offset) {
    return (static_cast<uint32_t>(buf[offset]) << 24U) |
           (static_cast<uint32_t>(buf[offset + 1U]) << 16U) |
           (static_cast<uint32_t>(buf[offset + 2U]) << 8U) |
           static_cast<uint32_t>(buf[offset + 3U]);
}

struct PngHeaderInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
};

std::optional<PngHeaderInfo> parse_png_wire_file(const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < kMinValidPngBytes) {
        return std::nullopt;
    }
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin())) {
        return std::nullopt;
    }
    if (read_be_u32(bytes, kIhdrLengthOffset) != kIhdrPayloadLength) {
        return std::nullopt;
    }
    if (!std::equal(kIhdrTag.begin(), kIhdrTag.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(kIhdrTagOffset))) {
        return std::nullopt;
    }
    if (!std::equal(kIendChunk.begin(), kIendChunk.end(),
                    bytes.end() - static_cast<std::ptrdiff_t>(kIendChunk.size()))) {
        return std::nullopt;
    }

    PngHeaderInfo info;
    info.width = read_be_u32(bytes, kIhdrWidthOffset);
    info.height = read_be_u32(bytes, kIhdrHeightOffset);
    info.bit_depth = bytes[kIhdrBitDepthOffset];
    info.color_type = bytes[kIhdrColorTypeOffset];
    return info;
}

void fill_bgra_rect(FPDF_BITMAP bmp, int x0, int y0, int w, int h, uint8_t b, uint8_t g, uint8_t r,
                    uint8_t a = 0xFFU) {
    auto* base = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    const int stride = FPDFBitmap_GetStride(bmp);
    constexpr int kBgraChannels = 4;
    for (int y = y0; y < y0 + h; ++y) {
        uint8_t* row = base + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = x0; x < x0 + w; ++x) {
            uint8_t* px = row + static_cast<std::ptrdiff_t>(x) * kBgraChannels;
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = a;
        }
    }
}

TEST(ImageExtractor, HandlesNullBitmapsAndUnwritablePathsSafely) {
    ScopedTempDir tmp;
    const fs::path null_out = tmp.path() / "null_bitmap.png";

    EXPECT_FALSE(pdf2md::write_bitmap_png(nullptr, null_out));
    EXPECT_FALSE(fs::exists(null_out));

    EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(nullptr), 0.0);

    const fs::path null_band_out = tmp.path() / "null_band.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(nullptr, null_band_out, 2, 0.01));
    EXPECT_FALSE(fs::exists(null_band_out));

    ScopedBitmap valid_bmp = make_bitmap(4, 4, FPDFBitmap_BGRA);
    ASSERT_NE(valid_bmp, nullptr);
    fill_bgra_rect(valid_bmp.get(), 0, 0, 4, 4, 0x20U, 0x40U, 0x80U);

    // Passing an existing directory as the output PNG file path must fail cleanly.
    EXPECT_FALSE(pdf2md::write_bitmap_png(valid_bmp.get(), tmp.path()));

    // Null bitmap hash is deterministic and distinct from a populated bitmap.
    EXPECT_EQ(pdf2md::hash_bitmap(nullptr), pdf2md::hash_bitmap(nullptr));
    EXPECT_NE(pdf2md::hash_bitmap(nullptr), pdf2md::hash_bitmap(valid_bmp.get()));
}

struct FormatWriteCase {
    const char* name;
    int format;
    int bytes_per_pixel;
    uint8_t expected_png_color_type;
};

constexpr uint8_t kPngColorGray = 0U;
constexpr uint8_t kPngColorRgb = 2U;
constexpr uint8_t kPngColorRgba = 6U;

constexpr FormatWriteCase kFormatCases[] = {
    {"gray_8bit", FPDFBitmap_Gray, 1, kPngColorGray},
    {"bgr_24bit", FPDFBitmap_BGR, 3, kPngColorRgb},
    {"bgrx_32bit_opaque_rgb", FPDFBitmap_BGRx, 4, kPngColorRgb},
    {"bgra_32bit_rgba", FPDFBitmap_BGRA, 4, kPngColorRgba},
};

constexpr std::size_t kExpectedFormatCaseCount = 4;
static_assert(std::size(kFormatCases) == kExpectedFormatCaseCount);

TEST(ImageExtractor, WritesValidPngAcrossBitmapFormatsAndUnicodePaths) {
    ScopedTempDir tmp;
    constexpr int kWidth = 7;   // Non-multiple of 4 exercises row-stride padding on Gray/BGR.
    constexpr int kHeight = 5;

    ASSERT_EQ(std::size(kFormatCases), kExpectedFormatCaseCount);
    for (const auto& tc : kFormatCases) {
        SCOPED_TRACE(tc.name);
        ScopedBitmap bmp = make_bitmap(kWidth, kHeight, tc.format);
        ASSERT_NE(bmp, nullptr);

        auto* buf = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp.get()));
        const int stride = FPDFBitmap_GetStride(bmp.get());
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth * tc.bytes_per_pixel; ++x) {
                buf[y * stride + x] = static_cast<uint8_t>((y * 31 + x * 17) & 0xFF);
            }
        }

        fs::path file_name(u8"figure_\u03B1_\u56F3_");
        file_name += tc.name;
        file_name += ".png";
        const fs::path out_path = tmp.path() / file_name;
        ASSERT_TRUE(pdf2md::write_bitmap_png(bmp.get(), out_path));

        const std::optional<PngHeaderInfo> png = parse_png_wire_file(out_path);
        ASSERT_TRUE(png.has_value());
        EXPECT_EQ(png->width, static_cast<uint32_t>(kWidth));
        EXPECT_EQ(png->height, static_cast<uint32_t>(kHeight));
        EXPECT_EQ(png->bit_depth, kBitsPerByte);
        EXPECT_EQ(png->color_type, tc.expected_png_color_type);
    }
}

TEST(ImageExtractor, HashBitmapIsSensitiveToSingleBitFlipAndInvariantToRowStride) {
    constexpr int kWidth = 5;
    constexpr int kHeight = 3;
    constexpr int kChannels = 3;
    constexpr int kActiveRowBytes = kWidth * kChannels;
    constexpr int kTightStride = 16;
    constexpr int kWideStride = 32;

    // Tight buffer padded with 0x00 vs wide buffer padded with 0xA5 garbage bytes.
    std::vector<uint8_t> tight_buf(static_cast<std::size_t>(kTightStride * kHeight), 0x00U);
    std::vector<uint8_t> wide_buf(static_cast<std::size_t>(kWideStride * kHeight), 0xA5U);

    for (int y = 0; y < kHeight; ++y) {
        for (int b = 0; b < kActiveRowBytes; ++b) {
            const auto val = static_cast<uint8_t>((y * 53 + b * 29 + 0x7FU) & 0xFFU);
            tight_buf[static_cast<std::size_t>(y * kTightStride + b)] = val;
            wide_buf[static_cast<std::size_t>(y * kWideStride + b)] = val;
        }
    }

    ScopedBitmap tight_bmp =
        make_bitmap(kWidth, kHeight, FPDFBitmap_BGR, tight_buf.data(), kTightStride);
    ScopedBitmap wide_bmp =
        make_bitmap(kWidth, kHeight, FPDFBitmap_BGR, wide_buf.data(), kWideStride);
    ASSERT_NE(tight_bmp, nullptr);
    ASSERT_NE(wide_bmp, nullptr);

    const uint64_t baseline_hash = pdf2md::hash_bitmap(tight_bmp.get());
    EXPECT_EQ(baseline_hash, pdf2md::hash_bitmap(wide_bmp.get()));

    // 1-bit flip at very first byte (0, 0).
    tight_buf[0] ^= 0x01U;
    const uint64_t first_bit_hash = pdf2md::hash_bitmap(tight_bmp.get());
    EXPECT_NE(first_bit_hash, baseline_hash);
    tight_buf[0] ^= 0x01U;
    EXPECT_EQ(pdf2md::hash_bitmap(tight_bmp.get()), baseline_hash);

    // 1-bit flip at very last active byte of last row (catches off-by-one loop bounds).
    const std::size_t last_active =
        static_cast<std::size_t>((kHeight - 1) * kTightStride + (kActiveRowBytes - 1));
    tight_buf[last_active] ^= 0x01U;
    EXPECT_NE(pdf2md::hash_bitmap(tight_bmp.get()), baseline_hash);
    tight_buf[last_active] ^= 0x01U;

    // Sign-bit byte seam: 0x7F vs 0x80 vs 0xFF in interior pixel byte.
    tight_buf[1] = 0x7FU;
    const uint64_t h_7f = pdf2md::hash_bitmap(tight_bmp.get());
    tight_buf[1] = 0x80U;
    const uint64_t h_80 = pdf2md::hash_bitmap(tight_bmp.get());
    tight_buf[1] = 0xFFU;
    const uint64_t h_ff = pdf2md::hash_bitmap(tight_bmp.get());
    EXPECT_NE(h_7f, h_80);
    EXPECT_NE(h_80, h_ff);
    EXPECT_NE(h_7f, h_ff);

    // Transposed dimensions with identical pixel count must not collide during deduplication.
    ScopedBitmap horiz_bmp = make_bitmap(4, 2, FPDFBitmap_BGRA);
    ScopedBitmap vert_bmp = make_bitmap(2, 4, FPDFBitmap_BGRA);
    ASSERT_NE(horiz_bmp, nullptr);
    ASSERT_NE(vert_bmp, nullptr);
    fill_bgra_rect(horiz_bmp.get(), 0, 0, 4, 2, 0x00U, 0x00U, 0x00U);
    fill_bgra_rect(vert_bmp.get(), 0, 0, 2, 4, 0x00U, 0x00U, 0x00U);
    EXPECT_NE(pdf2md::hash_bitmap(horiz_bmp.get()), pdf2md::hash_bitmap(vert_bmp.get()));
}

struct InkFractionCase {
    const char* name;
    int dark_pixels;
    double expected_fraction;
};

constexpr InkFractionCase kInkCases[] = {
    {"all_white_zero_ink", 0, 0.0},
    {"single_pixel_ink_seam", 1, 0.025},
    {"quarter_ink_fraction", 10, 0.25},
    {"all_black_full_ink", 40, 1.0},
};

constexpr std::size_t kExpectedInkCaseCount = 4;
static_assert(std::size(kInkCases) == kExpectedInkCaseCount);

TEST(ImageExtractor, ComputesBitmapInkFractionAcrossToneAndStrideSeams) {
    constexpr int kWidth = 10;
    constexpr int kHeight = 4;
    constexpr int kPaddedGrayStride = 16;

    ASSERT_EQ(std::size(kInkCases), kExpectedInkCaseCount);
    for (const auto& tc : kInkCases) {
        SCOPED_TRACE(tc.name);

        // Initialize entire buffer (including stride padding bytes 10..15) to 0x00 (black):
        // if bitmap_ink_fraction accidentally reads padding bytes, 0.0 and 0.25 will fail.
        std::vector<uint8_t> gray_buf(static_cast<std::size_t>(kPaddedGrayStride * kHeight),
                                      0x00U);
        int remaining_dark = tc.dark_pixels;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const uint8_t tone = (remaining_dark > 0) ? 0x00U : 0xFFU;
                if (remaining_dark > 0) {
                    --remaining_dark;
                }
                gray_buf[static_cast<std::size_t>(y * kPaddedGrayStride + x)] = tone;
            }
        }

        ScopedBitmap gray_bmp = make_bitmap(kWidth, kHeight, FPDFBitmap_Gray, gray_buf.data(),
                                            kPaddedGrayStride);
        ASSERT_NE(gray_bmp, nullptr);
        EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(gray_bmp.get()), tc.expected_fraction);

        // Verify matching fraction on 4-channel BGRA bitmap.
        ScopedBitmap bgra_bmp = make_bitmap(kWidth, kHeight, FPDFBitmap_BGRA);
        ASSERT_NE(bgra_bmp, nullptr);
        fill_bgra_rect(bgra_bmp.get(), 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);
        int bgra_dark = tc.dark_pixels;
        for (int y = 0; y < kHeight && bgra_dark > 0; ++y) {
            for (int x = 0; x < kWidth && bgra_dark > 0; ++x, --bgra_dark) {
                fill_bgra_rect(bgra_bmp.get(), x, y, 1, 1, 0x10U, 0x10U, 0x10U);
            }
        }
        EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(bgra_bmp.get()), tc.expected_fraction);
    }
}

TEST(ImageExtractor, ScanBandRejectsBlankOrUnderThresholdWithoutWritingFiles) {
    ScopedTempDir tmp;
    constexpr int kWidth = 100;
    constexpr int kHeight = 80;

    // Pure white scan band returns false without creating an output file.
    ScopedBitmap blank_bmp = make_bitmap(kWidth, kHeight, FPDFBitmap_BGRA);
    ASSERT_NE(blank_bmp, nullptr);
    fill_bgra_rect(blank_bmp.get(), 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);

    const fs::path blank_out = tmp.path() / "blank.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(blank_bmp.get(), blank_out, 4, 0.01));
    EXPECT_FALSE(fs::exists(blank_out));

    // Sparse 2-pixel dust corners below min_ink_fraction return false without creating a file.
    ScopedBitmap sparse_bmp = make_bitmap(kWidth, kHeight, FPDFBitmap_BGRA);
    ASSERT_NE(sparse_bmp, nullptr);
    fill_bgra_rect(sparse_bmp.get(), 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);
    fill_bgra_rect(sparse_bmp.get(), 10, 10, 1, 1, 0x00U, 0x00U, 0x00U);
    fill_bgra_rect(sparse_bmp.get(), 90, 70, 1, 1, 0x00U, 0x00U, 0x00U);

    const fs::path sparse_out = tmp.path() / "sparse.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(sparse_bmp.get(), sparse_out, 2, 0.05));
    EXPECT_FALSE(fs::exists(sparse_out));

    // Top-edge text bleed strip with blank paper below trims to empty and writes nothing.
    ScopedBitmap bleed_only_bmp = make_bitmap(kWidth, kHeight, FPDFBitmap_BGRA);
    ASSERT_NE(bleed_only_bmp, nullptr);
    fill_bgra_rect(bleed_only_bmp.get(), 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);
    fill_bgra_rect(bleed_only_bmp.get(), 15, 0, 60, 4, 0x00U, 0x00U, 0x00U);

    const fs::path bleed_only_out = tmp.path() / "bleed_only.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(bleed_only_bmp.get(), bleed_only_out, 2, 0.01));
    EXPECT_FALSE(fs::exists(bleed_only_out));
}

TEST(ImageExtractor, ScanBandTrimsEdgeBleedAndCropsToInkBoundingBoxPlusPadding) {
    ScopedTempDir tmp;
    constexpr int kCanvasW = 120;
    constexpr int kCanvasH = 120;
    constexpr int kFigX = 30;
    constexpr int kFigY = 35;
    constexpr int kFigW = 50;
    constexpr int kFigH = 40;

    ScopedBitmap band_bmp = make_bitmap(kCanvasW, kCanvasH, FPDFBitmap_BGRA);
    ASSERT_NE(band_bmp, nullptr);
    fill_bgra_rect(band_bmp.get(), 0, 0, kCanvasW, kCanvasH, 0xFFU, 0xFFU, 0xFFU);

    // Neighbouring text-line bleed hugging top (rows 0..3) and bottom (rows 116..119),
    // separated from the central figure by wide white gaps.
    fill_bgra_rect(band_bmp.get(), 5, 0, 100, 4, 0x00U, 0x00U, 0x00U);
    fill_bgra_rect(band_bmp.get(), 5, 116, 100, 4, 0x00U, 0x00U, 0x00U);

    // Central dark figure at [30..80) x [35..75) -> exact 50x40 ink bbox.
    fill_bgra_rect(band_bmp.get(), kFigX, kFigY, kFigW, kFigH, 0x00U, 0x00U, 0x00U);

    struct CropPadCase {
        const char* name;
        int pad;
        uint32_t expected_w;
        uint32_t expected_h;
    };

    constexpr CropPadCase kPadCases[] = {
        {"zero_padding_exact_figure_bbox", 0, 50U, 40U},
        {"interior_padding_expands_all_four_sides", 4, 58U, 48U},
    };
    constexpr std::size_t kExpectedPadCaseCount = 2;
    ASSERT_EQ(std::size(kPadCases), kExpectedPadCaseCount);

    for (const auto& tc : kPadCases) {
        SCOPED_TRACE(tc.name);
        const fs::path out_path = tmp.path() / (std::string(tc.name) + ".png");
        ASSERT_TRUE(pdf2md::write_scan_band_png(band_bmp.get(), out_path, tc.pad, 0.01));

        const std::optional<PngHeaderInfo> png = parse_png_wire_file(out_path);
        ASSERT_TRUE(png.has_value());
        EXPECT_EQ(png->width, tc.expected_w);
        EXPECT_EQ(png->height, tc.expected_h);
        EXPECT_EQ(png->bit_depth, kBitsPerByte);
    }

    // Oversized padding on a clean figure canvas clamps to exact [0..width) x [0..height) bounds.
    ScopedBitmap clean_bmp = make_bitmap(kCanvasW, kCanvasH, FPDFBitmap_BGRA);
    ASSERT_NE(clean_bmp, nullptr);
    fill_bgra_rect(clean_bmp.get(), 0, 0, kCanvasW, kCanvasH, 0xFFU, 0xFFU, 0xFFU);
    fill_bgra_rect(clean_bmp.get(), kFigX, kFigY, kFigW, kFigH, 0x00U, 0x00U, 0x00U);

    const fs::path clamped_out = tmp.path() / "oversized_pad_clamped.png";
    ASSERT_TRUE(pdf2md::write_scan_band_png(clean_bmp.get(), clamped_out, 200, 0.01));
    const std::optional<PngHeaderInfo> clamped_png = parse_png_wire_file(clamped_out);
    ASSERT_TRUE(clamped_png.has_value());
    EXPECT_EQ(clamped_png->width, static_cast<uint32_t>(kCanvasW));
    EXPECT_EQ(clamped_png->height, static_cast<uint32_t>(kCanvasH));
}

}  // namespace
