#include "pdf2md/utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pdf2md {

namespace {

constexpr char32_t kTwoByteMin = 0x80U;
constexpr char32_t kThreeByteMin = 0x800U;
constexpr char32_t kFourByteMin = 0x10000U;
constexpr char32_t kMaxUnicodeCodePoint = 0x10FFFFU;

constexpr char32_t kSurrogateHighStart = 0xD800U;
constexpr char32_t kSurrogateHighEnd = 0xDBFFU;
constexpr char32_t kSurrogateLowStart = 0xDC00U;
constexpr char32_t kSurrogateLowEnd = 0xDFFFU;
constexpr char32_t kSurrogateShift = 10U;

constexpr char32_t kTwoByteLead = 0xC0U;
constexpr char32_t kThreeByteLead = 0xE0U;
constexpr char32_t kFourByteLead = 0xF0U;
constexpr char32_t kContinuationLead = 0x80U;
constexpr char32_t kContinuationMask = 0x3FU;

constexpr std::size_t kMaxInitialUtf16Reserve = 4096U;
constexpr std::uint32_t kHighByteShift = 8U;

constexpr char32_t decode_utf16le_unit(const unsigned char* pair) noexcept {
    return static_cast<char32_t>(static_cast<std::uint32_t>(pair[0]) |
                                 (static_cast<std::uint32_t>(pair[1]) << kHighByteShift));
}

}  // namespace

void append_utf8(std::string& out, char32_t cp) {
    if (cp >= kSurrogateHighStart && cp <= kSurrogateLowEnd) {
        return;  // UTF-16 surrogates are never valid scalar values
    }
    if (cp < kTwoByteMin) {
        out += static_cast<char>(cp);
        return;
    }
    if (cp < kThreeByteMin) {
        out += static_cast<char>(kTwoByteLead | (cp >> 6U));
        out += static_cast<char>(kContinuationLead | (cp & kContinuationMask));
        return;
    }
    if (cp < kFourByteMin) {
        out += static_cast<char>(kThreeByteLead | (cp >> 12U));
        out += static_cast<char>(kContinuationLead | ((cp >> 6U) & kContinuationMask));
        out += static_cast<char>(kContinuationLead | (cp & kContinuationMask));
        return;
    }
    if (cp <= kMaxUnicodeCodePoint) {
        out += static_cast<char>(kFourByteLead | (cp >> 18U));
        out += static_cast<char>(kContinuationLead | ((cp >> 12U) & kContinuationMask));
        out += static_cast<char>(kContinuationLead | ((cp >> 6U) & kContinuationMask));
        out += static_cast<char>(kContinuationLead | (cp & kContinuationMask));
    }
    // Code points above U+10FFFF are dropped.
}

std::string to_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char32_t cp : s) {
        append_utf8(out, cp);
    }
    return out;
}

std::string utf16le_to_utf8(const unsigned char* data, std::size_t bytes) {
    if (data == nullptr || bytes < 2U) {
        return {};
    }
    const std::size_t code_units = bytes / 2U;
    std::string out;
    out.reserve((std::min)(code_units, kMaxInitialUtf16Reserve));
    for (std::size_t i = 0; i < code_units; ++i) {
        const char32_t u = decode_utf16le_unit(data + 2U * i);
        if (u == 0U) {
            break;
        }
        if (u >= kSurrogateHighStart && u <= kSurrogateHighEnd) {
            if (i + 1U >= code_units) {
                continue;
            }
            const char32_t lo = decode_utf16le_unit(data + 2U * (i + 1U));
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

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string path_to_utf8_generic(const std::filesystem::path& p) {
    const std::u8string u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace pdf2md
