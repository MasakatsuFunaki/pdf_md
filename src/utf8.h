#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pdf2md {

namespace detail {

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

}  // namespace detail

inline void append_utf8(std::string& out, char32_t cp) {
    if (cp >= detail::kSurrogateHighStart && cp <= detail::kSurrogateLowEnd) {
        return;  // UTF-16 surrogates are never valid scalar values
    }
    if (cp < detail::kTwoByteMin) {
        out += static_cast<char>(cp);
        return;
    }
    if (cp < detail::kThreeByteMin) {
        out += static_cast<char>(detail::kTwoByteLead | (cp >> 6U));
        out += static_cast<char>(detail::kContinuationLead | (cp & detail::kContinuationMask));
        return;
    }
    if (cp < detail::kFourByteMin) {
        out += static_cast<char>(detail::kThreeByteLead | (cp >> 12U));
        out += static_cast<char>(detail::kContinuationLead | ((cp >> 6U) & detail::kContinuationMask));
        out += static_cast<char>(detail::kContinuationLead | (cp & detail::kContinuationMask));
        return;
    }
    if (cp <= detail::kMaxUnicodeCodePoint) {
        out += static_cast<char>(detail::kFourByteLead | (cp >> 18U));
        out += static_cast<char>(detail::kContinuationLead | ((cp >> 12U) & detail::kContinuationMask));
        out += static_cast<char>(detail::kContinuationLead | ((cp >> 6U) & detail::kContinuationMask));
        out += static_cast<char>(detail::kContinuationLead | (cp & detail::kContinuationMask));
    }
    // Code points above U+10FFFF are dropped.
}

inline std::string to_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char32_t cp : s) {
        append_utf8(out, cp);
    }
    return out;
}

// Decodes UTF-16LE bytes (as returned by FPDF_GetMetaText) to UTF-8,
// stopping at the first terminating NUL code unit. Trailing odd bytes and
// unpaired surrogates are ignored.
inline std::string utf16le_to_utf8(const unsigned char* data, std::size_t bytes) {
    if (data == nullptr || bytes < 2U) {
        return {};
    }
    const std::size_t code_units = bytes / 2U;
    std::string out;
    out.reserve((std::min)(code_units, detail::kMaxInitialUtf16Reserve));
    for (std::size_t i = 0; i < code_units; ++i) {
        const char32_t u = detail::decode_utf16le_unit(data + 2U * i);
        if (u == 0U) {
            break;
        }
        if (u >= detail::kSurrogateHighStart && u <= detail::kSurrogateHighEnd) {
            if (i + 1U >= code_units) {
                continue;
            }
            const char32_t lo = detail::decode_utf16le_unit(data + 2U * (i + 1U));
            if (lo < detail::kSurrogateLowStart || lo > detail::kSurrogateLowEnd) {
                continue;
            }
            const char32_t combined = detail::kFourByteMin +
                                      ((u - detail::kSurrogateHighStart) << detail::kSurrogateShift) +
                                      (lo - detail::kSurrogateLowStart);
            ++i;
            append_utf8(out, combined);
            continue;
        }
        if (u >= detail::kSurrogateLowStart && u <= detail::kSurrogateLowEnd) {
            continue;
        }
        append_utf8(out, u);
    }
    return out;
}

constexpr bool is_space_cp(char32_t cp) noexcept {
    switch (cp) {
        case 0x09U:
        case 0x20U:
        case 0xA0U:
        case 0x1680U:
        case 0x2000U:
        case 0x2001U:
        case 0x2002U:
        case 0x2003U:
        case 0x2004U:
        case 0x2005U:
        case 0x2006U:
        case 0x2007U:
        case 0x2008U:
        case 0x2009U:
        case 0x200AU:
        case 0x202FU:
        case 0x205FU:
        case 0x3000U:
            return true;
        default:
            return false;
    }
}

constexpr bool is_cjk_cp(char32_t cp) noexcept {
    return (cp >= 0x2E80U && cp <= 0x9FFFU) || (cp >= 0xF900U && cp <= 0xFAFFU) ||
           (cp >= 0xFF00U && cp <= 0xFFEFU) || (cp >= 0xAC00U && cp <= 0xD7AFU) ||
           (cp >= 0x20000U && cp <= 0x2FA1FU);
}

constexpr bool is_ascii_digit(char32_t cp) noexcept { return cp >= U'0' && cp <= U'9'; }
constexpr bool is_ascii_lower(char32_t cp) noexcept { return cp >= U'a' && cp <= U'z'; }
constexpr bool is_ascii_upper(char32_t cp) noexcept { return cp >= U'A' && cp <= U'Z'; }
constexpr bool is_ascii_alpha(char32_t cp) noexcept {
    return is_ascii_lower(cp) || is_ascii_upper(cp);
}

}  // namespace pdf2md
