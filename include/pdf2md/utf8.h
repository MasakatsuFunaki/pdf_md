#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace pdf2md {

inline constexpr char32_t kSoftHyphenCp = 0x00ADU;
inline constexpr char32_t kUnicodeHyphenCp = 0x2010U;

void append_utf8(std::string& out, char32_t cp);
std::string to_utf8(std::u32string_view s);

// Decodes UTF-16LE bytes (as returned by FPDF_GetMetaText) to UTF-8,
// stopping at the first terminating NUL code unit. Trailing odd bytes and
// unpaired surrogates are ignored.
std::string utf16le_to_utf8(const unsigned char* data, std::size_t bytes);

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

constexpr bool is_line_break_cp(char32_t cp) noexcept {
    switch (cp) {
        case U'\n':
        case U'\r':
        case 0x0085U:  // NEXT LINE (NEL)
        case 0x2028U:  // LINE SEPARATOR
        case 0x2029U:  // PARAGRAPH SEPARATOR
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

constexpr bool is_greek_cp(char32_t cp) noexcept {
    return (cp >= 0x0391U && cp <= 0x03A9U) || (cp >= 0x03B1U && cp <= 0x03C9U) ||
           cp == 0x03D1U || cp == 0x03D5U || cp == 0x03D6U || cp == 0x03F1U ||
           cp == 0x03F5U;  // theta/phi/pi/rho and lunate-epsilon variants
}

constexpr bool is_letter_cp(char32_t cp) noexcept {
    return is_ascii_alpha(cp) || is_greek_cp(cp);
}

constexpr bool is_bullet_cp(char32_t cp) noexcept {
    switch (cp) {
        case 0x00B7U:  // middle dot
        case 0x2022U:  // bullet
        case 0x2023U:  // triangular bullet
        case 0x2043U:  // hyphen bullet
        case 0x2219U:  // bullet operator
        case 0x25A0U:  // black square
        case 0x25AAU:  // black small square
        case 0x25CBU:  // white circle
        case 0x25CFU:  // black circle
        case 0x25E6U:  // white bullet
        case U'*':
            return true;
        default:
            return false;
    }
}

constexpr bool is_dashy_cp(char32_t cp) noexcept {
    return cp == U'-' || cp == 0x2013U || cp == 0x2014U;
}

constexpr char ascii_tolower(char32_t cp) noexcept {
    if (cp >= U'A' && cp <= U'Z') {
        return static_cast<char>(cp - U'A' + U'a');
    }
    return static_cast<char>(cp);
}

std::filesystem::path path_from_utf8(std::string_view utf8);

// path::string() can throw for non-ANSI-representable paths on Windows;
// always display paths as UTF-8 instead.
std::string path_to_utf8(const std::filesystem::path& p);

// UTF-8 with forward slashes, for paths embedded in the markdown output.
std::string path_to_utf8_generic(const std::filesystem::path& p);

}  // namespace pdf2md
