#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pdf2md {

inline void append_utf8(std::string& out, char32_t cp) {
    if (cp >= 0xD800 && cp < 0xE000) return;  // surrogates are never valid UTF-8
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    // Invalid code points are dropped.
}

inline std::string to_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t cp : s) append_utf8(out, cp);
    return out;
}

// Decodes UTF-16LE bytes (as returned by FPDF_GetMetaText) to UTF-8,
// stopping at the terminating NUL.
inline std::string utf16le_to_utf8(const unsigned char* data, size_t bytes) {
    std::string out;
    size_t n = bytes / 2;
    for (size_t i = 0; i < n; ++i) {
        char32_t u = static_cast<char32_t>(data[2 * i] | (data[2 * i + 1] << 8));
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) {
            char32_t lo = static_cast<char32_t>(data[2 * i + 2] | (data[2 * i + 3] << 8));
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                continue;  // unpaired surrogate
            }
        } else if (u >= 0xDC00 && u < 0xE000) {
            continue;  // unpaired low surrogate
        }
        append_utf8(out, u);
    }
    return out;
}

inline bool is_space_cp(char32_t cp) {
    switch (cp) {
        case 0x09: case 0x20: case 0xA0: case 0x1680:
        case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
        case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
        case 0x200A: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return false;
    }
}

inline bool is_cjk_cp(char32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

inline bool is_ascii_digit(char32_t cp) { return cp >= U'0' && cp <= U'9'; }
inline bool is_ascii_lower(char32_t cp) { return cp >= U'a' && cp <= U'z'; }
inline bool is_ascii_upper(char32_t cp) { return cp >= U'A' && cp <= U'Z'; }
inline bool is_ascii_alpha(char32_t cp) { return is_ascii_lower(cp) || is_ascii_upper(cp); }

}  // namespace pdf2md
