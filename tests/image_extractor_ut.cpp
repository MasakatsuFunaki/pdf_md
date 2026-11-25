#include "image_extractor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

pdf2md::RasterImage make_bitmap(int width, int height, pdf2md::PixelFormat format) {
    const int bpp = pdf2md::bytes_per_pixel(format);
    const int aligned_stride = ((width * bpp + 3) / 4) * 4;
    return pdf2md::RasterImage(width, height, format, aligned_stride);
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

void fill_bgra_rect(pdf2md::RasterImage& bmp, int x0, int y0, int w, int h, uint8_t b, uint8_t g,
                    uint8_t r, uint8_t a = 0xFFU) {
    uint8_t* base = bmp.pixels.data();
    const int stride = bmp.stride;
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
    std::vector<uint8_t> backing_buf(64U, 0x40U);

    struct InvalidGeometryCase {
        const char* name;
        bool use_backing;
        int width;
        int height;
        int stride;
        pdf2md::PixelFormat format;
    };

    constexpr InvalidGeometryCase kInvalidCases[] = {
        {"default_null_view", false, 0, 0, 0, pdf2md::PixelFormat::BGRA},
        {"null_data_with_positive_dimensions", false, 4, 4, 16, pdf2md::PixelFormat::BGRA},
        {"zero_width", true, 0, 4, 16, pdf2md::PixelFormat::BGRA},
        {"negative_width", true, -4, 4, 16, pdf2md::PixelFormat::BGRA},
        {"zero_height", true, 4, 0, 16, pdf2md::PixelFormat::BGRA},
        {"negative_height", true, 4, -4, 16, pdf2md::PixelFormat::BGRA},
        {"zero_stride", true, 4, 4, 0, pdf2md::PixelFormat::BGRA},
        {"negative_stride", true, 4, 4, -16, pdf2md::PixelFormat::BGRA},
        {"undersized_stride_below_active_row_bytes", true, 4, 4, 15, pdf2md::PixelFormat::BGRA},
    };
    constexpr std::size_t kExpectedInvalidCaseCount = 9;
    ASSERT_EQ(std::size(kInvalidCases), kExpectedInvalidCaseCount);

    for (const auto& tc : kInvalidCases) {
        SCOPED_TRACE(tc.name);
        const pdf2md::BitmapView invalid_bmp{
            tc.use_backing ? backing_buf.data() : nullptr,
            tc.width,
            tc.height,
            tc.stride,
            tc.format,
        };
        const fs::path out_png = tmp.path() / (std::string(tc.name) + ".png");
        const fs::path out_band = tmp.path() / (std::string(tc.name) + "_band.png");

        EXPECT_FALSE(pdf2md::write_bitmap_png(invalid_bmp, out_png));
        EXPECT_FALSE(fs::exists(out_png));
        EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(invalid_bmp), 0.0);
        EXPECT_FALSE(pdf2md::write_scan_band_png(invalid_bmp, out_band, 2, 0.01));
        EXPECT_FALSE(fs::exists(out_band));
        EXPECT_EQ(pdf2md::hash_bitmap(invalid_bmp), pdf2md::hash_bitmap(invalid_bmp));
    }

    // RasterImage constructor guards invalid dimensions/strides and defaults stride = 0 to tight.
    const pdf2md::RasterImage default_img;
    EXPECT_TRUE(default_img.empty());
    EXPECT_EQ(default_img.view().data, nullptr);

    const pdf2md::RasterImage neg_stride_img(4, 4, pdf2md::PixelFormat::BGRA, -16);
    EXPECT_TRUE(neg_stride_img.empty());
    EXPECT_EQ(neg_stride_img.view().data, nullptr);

    const pdf2md::RasterImage tight_bgr_img(5, 3, pdf2md::PixelFormat::BGR);
    EXPECT_FALSE(tight_bgr_img.empty());
    EXPECT_EQ(tight_bgr_img.stride, 15);
    EXPECT_EQ(tight_bgr_img.pixels.size(), std::size_t{45});

    pdf2md::RasterImage valid_bmp = make_bitmap(4, 4, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(valid_bmp.empty());
    fill_bgra_rect(valid_bmp, 0, 0, 4, 4, 0x20U, 0x40U, 0x80U);

    // Passing an existing directory as the output PNG file path must fail cleanly.
    EXPECT_FALSE(pdf2md::write_bitmap_png(valid_bmp.view(), tmp.path()));

    const pdf2md::BitmapView null_bmp{};
    EXPECT_NE(pdf2md::hash_bitmap(null_bmp), pdf2md::hash_bitmap(valid_bmp.view()));
}

struct FormatWriteCase {
    const char* name;
    pdf2md::PixelFormat format;
    int bytes_per_pixel;
    uint8_t expected_png_color_type;
};

constexpr uint8_t kPngColorGray = 0U;
constexpr uint8_t kPngColorRgb = 2U;
constexpr uint8_t kPngColorRgba = 6U;

constexpr FormatWriteCase kFormatCases[] = {
    {"gray_8bit", pdf2md::PixelFormat::Gray, 1, kPngColorGray},
    {"bgr_24bit", pdf2md::PixelFormat::BGR, 3, kPngColorRgb},
    {"bgrx_32bit_opaque_rgb", pdf2md::PixelFormat::BGRx, 4, kPngColorRgb},
    {"bgra_32bit_rgba", pdf2md::PixelFormat::BGRA, 4, kPngColorRgba},
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
        pdf2md::RasterImage bmp = make_bitmap(kWidth, kHeight, tc.format);
        ASSERT_FALSE(bmp.empty());

        uint8_t* buf = bmp.pixels.data();
        const int stride = bmp.stride;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth * tc.bytes_per_pixel; ++x) {
                buf[y * stride + x] = static_cast<uint8_t>((y * 31 + x * 17) & 0xFF);
            }
        }

        fs::path file_name(u8"figure_\u03B1_\u56F3_");
        file_name += tc.name;
        file_name += ".png";
        const fs::path out_path = tmp.path() / file_name;
        ASSERT_TRUE(pdf2md::write_bitmap_png(bmp.view(), out_path));

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
    constexpr int kWideStride = 40;

    ASSERT_EQ(std::size(kFormatCases), kExpectedFormatCaseCount);
    for (const auto& fc : kFormatCases) {
        SCOPED_TRACE(fc.name);
        const int active_row_bytes = kWidth * fc.bytes_per_pixel;
        const int tight_stride = active_row_bytes;

        std::vector<uint8_t> tight_buf(static_cast<std::size_t>(tight_stride * kHeight), 0x00U);
        std::vector<uint8_t> wide_buf(static_cast<std::size_t>(kWideStride * kHeight), 0xA5U);

        for (int y = 0; y < kHeight; ++y) {
            for (int b = 0; b < active_row_bytes; ++b) {
                const auto val = static_cast<uint8_t>((y * 53 + b * 29 + 0x7FU) & 0xFFU);
                tight_buf[static_cast<std::size_t>(y * tight_stride + b)] = val;
                wide_buf[static_cast<std::size_t>(y * kWideStride + b)] = val;
            }
        }

        const pdf2md::BitmapView tight_bmp{tight_buf.data(), kWidth, kHeight, tight_stride,
                                           fc.format};
        const pdf2md::BitmapView wide_bmp{wide_buf.data(), kWidth, kHeight, kWideStride,
                                          fc.format};

        const uint64_t baseline_hash = pdf2md::hash_bitmap(tight_bmp);
        EXPECT_EQ(baseline_hash, pdf2md::hash_bitmap(wide_bmp));

        // 1-bit flip at first active byte (0, 0).
        tight_buf[0] ^= 0x01U;
        EXPECT_NE(pdf2md::hash_bitmap(tight_bmp), baseline_hash);
        tight_buf[0] ^= 0x01U;
        EXPECT_EQ(pdf2md::hash_bitmap(tight_bmp), baseline_hash);

        // 1-bit flip at last active color byte of last row (catches off-by-one loop bounds;
        // for BGRx the 4th byte of each pixel is unused padding X, so flip the R channel).
        const int last_color_offset =
            (fc.format == pdf2md::PixelFormat::BGRx) ? (active_row_bytes - 2)
                                                     : (active_row_bytes - 1);
        const std::size_t last_active =
            static_cast<std::size_t>((kHeight - 1) * tight_stride + last_color_offset);
        tight_buf[last_active] ^= 0x01U;
        EXPECT_NE(pdf2md::hash_bitmap(tight_bmp), baseline_hash);
        tight_buf[last_active] ^= 0x01U;

        // Sign-bit byte seam: 0x7F vs 0x80 vs 0xFF in first pixel channel.
        tight_buf[0] = 0x7FU;
        const uint64_t h_7f = pdf2md::hash_bitmap(tight_bmp);
        tight_buf[0] = 0x80U;
        const uint64_t h_80 = pdf2md::hash_bitmap(tight_bmp);
        tight_buf[0] = 0xFFU;
        const uint64_t h_ff = pdf2md::hash_bitmap(tight_bmp);
        EXPECT_NE(h_7f, h_80);
        EXPECT_NE(h_80, h_ff);
        EXPECT_NE(h_7f, h_ff);
    }

    // Transposed dimensions with identical pixel count must not collide during deduplication.
    pdf2md::RasterImage horiz_bmp = make_bitmap(4, 2, pdf2md::PixelFormat::BGRA);
    pdf2md::RasterImage vert_bmp = make_bitmap(2, 4, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(horiz_bmp.empty());
    ASSERT_FALSE(vert_bmp.empty());
    fill_bgra_rect(horiz_bmp, 0, 0, 4, 2, 0x00U, 0x00U, 0x00U);
    fill_bgra_rect(vert_bmp, 0, 0, 2, 4, 0x00U, 0x00U, 0x00U);
    EXPECT_NE(pdf2md::hash_bitmap(horiz_bmp.view()), pdf2md::hash_bitmap(vert_bmp.view()));
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
    constexpr int kPaddedStride = 48;

    ASSERT_EQ(std::size(kInkCases), kExpectedInkCaseCount);
    for (const auto& tc : kInkCases) {
        SCOPED_TRACE(tc.name);

        ASSERT_EQ(std::size(kFormatCases), kExpectedFormatCaseCount);
        for (const auto& fc : kFormatCases) {
            SCOPED_TRACE(fc.name);
            // Fill entire padded buffer with 0x00 so reading stride padding or BGRx's
            // unused X byte (set to 0x00 on white pixels) fails immediately.
            std::vector<uint8_t> buf(static_cast<std::size_t>(kPaddedStride * kHeight), 0x00U);
            int remaining_dark = tc.dark_pixels;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const bool is_dark = remaining_dark > 0;
                    if (is_dark) {
                        --remaining_dark;
                    }
                    const uint8_t tone = is_dark ? 0x10U : 0xFFU;
                    uint8_t* px = buf.data() +
                                  static_cast<std::ptrdiff_t>(y * kPaddedStride +
                                                              x * fc.bytes_per_pixel);
                    if (fc.format == pdf2md::PixelFormat::Gray) {
                        px[0] = tone;
                    } else if (fc.format == pdf2md::PixelFormat::BGR) {
                        px[0] = tone;
                        px[1] = tone;
                        px[2] = tone;
                    } else if (fc.format == pdf2md::PixelFormat::BGRx) {
                        px[0] = tone;
                        px[1] = tone;
                        px[2] = tone;
                        px[3] = 0x00U;  // Unused X channel must not darken white BGRx pixels.
                    } else {
                        px[0] = tone;
                        px[1] = tone;
                        px[2] = tone;
                        px[3] = 0xFFU;
                    }
                }
            }
            const pdf2md::BitmapView view{buf.data(), kWidth, kHeight, kPaddedStride, fc.format};
            EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(view), tc.expected_fraction);
        }
    }

    // Sign-bit tone check (0x7F and 0x80 both count as dark ink, while 0xFF is blank paper).
    const std::array<uint8_t, 3> sign_seam_row = {0x7FU, 0x80U, 0xFFU};
    const pdf2md::BitmapView sign_seam_bmp{sign_seam_row.data(), 3, 1, 3,
                                           pdf2md::PixelFormat::Gray};
    EXPECT_DOUBLE_EQ(pdf2md::bitmap_ink_fraction(sign_seam_bmp), 2.0 / 3.0);
}

TEST(ImageExtractor, ScanBandRejectsBlankOrUnderThresholdWithoutWritingFiles) {
    ScopedTempDir tmp;
    constexpr int kWidth = 100;
    constexpr int kHeight = 80;

    // Pure white scan band returns false without creating an output file.
    pdf2md::RasterImage blank_bmp = make_bitmap(kWidth, kHeight, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(blank_bmp.empty());
    fill_bgra_rect(blank_bmp, 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);

    const fs::path blank_out = tmp.path() / "blank.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(blank_bmp.view(), blank_out, 4, 0.01));
    EXPECT_FALSE(fs::exists(blank_out));

    // Top-edge text bleed strip with blank paper below trims to empty and writes nothing.
    pdf2md::RasterImage bleed_only_bmp = make_bitmap(kWidth, kHeight, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(bleed_only_bmp.empty());
    fill_bgra_rect(bleed_only_bmp, 0, 0, kWidth, kHeight, 0xFFU, 0xFFU, 0xFFU);
    fill_bgra_rect(bleed_only_bmp, 15, 0, 60, 4, 0x00U, 0x00U, 0x00U);

    const fs::path bleed_only_out = tmp.path() / "bleed_only.png";
    EXPECT_FALSE(pdf2md::write_scan_band_png(bleed_only_bmp.view(), bleed_only_out, 2, 0.01));
    EXPECT_FALSE(fs::exists(bleed_only_out));

    // Exact min_ink_fraction seam (B - 1, B, B + 1 dark pixels) on a 50x40 (2000 px) figure
    // with continuous left/right borders (zero white gaps, cropped bbox == full 50x40 canvas)
    // against min_ink_fraction = 0.125 (exact binary float 250 / 2000).
    struct ThresholdSeamCase {
        const char* name;
        int total_dark_pixels;
        bool expected_written;
    };
    constexpr ThresholdSeamCase kSeamCases[] = {
        {"below_min_ink_fraction_b_minus_1", 249, false},
        {"exact_min_ink_fraction_seam_b", 250, true},
        {"above_min_ink_fraction_b_plus_1", 251, true},
    };
    constexpr std::size_t kExpectedSeamCount = 3;
    ASSERT_EQ(std::size(kSeamCases), kExpectedSeamCount);

    constexpr int kSeamW = 50;
    constexpr int kSeamH = 40;
    constexpr int kLeftBarW = 6;  // 6 * 40 = 240 px: uniform 12% density on every row, zero gaps.
    constexpr int kLeftBarPixels = kLeftBarW * kSeamH;
    constexpr double kThreshold = 0.125;

    for (const auto& tc : kSeamCases) {
        SCOPED_TRACE(tc.name);
        pdf2md::RasterImage seam_bmp = make_bitmap(kSeamW, kSeamH, pdf2md::PixelFormat::BGRA);
        ASSERT_FALSE(seam_bmp.empty());
        fill_bgra_rect(seam_bmp, 0, 0, kSeamW, kSeamH, 0xFFU, 0xFFU, 0xFFU);

        fill_bgra_rect(seam_bmp, 0, 0, kLeftBarW, kSeamH, 0x00U, 0x00U, 0x00U);
        const int right_edge_pixels = tc.total_dark_pixels - kLeftBarPixels;
        fill_bgra_rect(seam_bmp, kSeamW - 1, 10, 1, right_edge_pixels, 0x00U, 0x00U, 0x00U);

        const fs::path seam_out = tmp.path() / (std::string(tc.name) + ".png");
        EXPECT_EQ(pdf2md::write_scan_band_png(seam_bmp.view(), seam_out, 0, kThreshold),
                  tc.expected_written);
        EXPECT_EQ(fs::exists(seam_out), tc.expected_written);
    }
}

TEST(ImageExtractor, ScanBandTrimsEdgeBleedAndCropsToInkBoundingBoxPlusPadding) {
    ScopedTempDir tmp;
    constexpr int kCanvasW = 120;
    constexpr int kCanvasH = 120;
    constexpr int kFigX = 30;
    constexpr int kFigY = 35;
    constexpr int kFigW = 50;
    constexpr int kFigH = 40;

    pdf2md::RasterImage band_bmp = make_bitmap(kCanvasW, kCanvasH, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(band_bmp.empty());
    fill_bgra_rect(band_bmp, 0, 0, kCanvasW, kCanvasH, 0xFFU, 0xFFU, 0xFFU);

    // Neighbouring text-line bleed hugging top (rows 0..3) and bottom (rows 116..119),
    // separated from the central figure by wide white gaps.
    fill_bgra_rect(band_bmp, 5, 0, 100, 4, 0x00U, 0x00U, 0x00U);
    fill_bgra_rect(band_bmp, 5, 116, 100, 4, 0x00U, 0x00U, 0x00U);

    // Central dark figure at [30..80) x [35..75) -> exact 50x40 ink bbox.
    fill_bgra_rect(band_bmp, kFigX, kFigY, kFigW, kFigH, 0x00U, 0x00U, 0x00U);

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
        ASSERT_TRUE(pdf2md::write_scan_band_png(band_bmp.view(), out_path, tc.pad, 0.01));

        const std::optional<PngHeaderInfo> png = parse_png_wire_file(out_path);
        ASSERT_TRUE(png.has_value());
        EXPECT_EQ(png->width, tc.expected_w);
        EXPECT_EQ(png->height, tc.expected_h);
        EXPECT_EQ(png->bit_depth, kBitsPerByte);
    }

    // Oversized padding on a clean figure canvas clamps to exact [0..width) x [0..height) bounds.
    pdf2md::RasterImage clean_bmp = make_bitmap(kCanvasW, kCanvasH, pdf2md::PixelFormat::BGRA);
    ASSERT_FALSE(clean_bmp.empty());
    fill_bgra_rect(clean_bmp, 0, 0, kCanvasW, kCanvasH, 0xFFU, 0xFFU, 0xFFU);
    fill_bgra_rect(clean_bmp, kFigX, kFigY, kFigW, kFigH, 0x00U, 0x00U, 0x00U);

    const fs::path clamped_out = tmp.path() / "oversized_pad_clamped.png";
    ASSERT_TRUE(pdf2md::write_scan_band_png(clean_bmp.view(), clamped_out, 200, 0.01));
    const std::optional<PngHeaderInfo> clamped_png = parse_png_wire_file(clamped_out);
    ASSERT_TRUE(clamped_png.has_value());
    EXPECT_EQ(clamped_png->width, static_cast<uint32_t>(kCanvasW));
    EXPECT_EQ(clamped_png->height, static_cast<uint32_t>(kCanvasH));
}

}  // namespace
