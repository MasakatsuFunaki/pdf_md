#include "utf8.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using namespace std::string_view_literals;

struct Utf8BoundaryCase {
    const char* name;
    char32_t cp;
    std::string_view expected;
};

constexpr Utf8BoundaryCase kUtf8Cases[] = {
    // 1-byte partition [0x0000..0x007F] and sign-bit seam B-1 (0x7F)
    {"nul_min_1byte", 0x0000U, "\x00"sv},
    {"ascii_interior", 0x0041U, "A"sv},
    {"ascii_max_sign_seam_low", 0x007FU, "\x7F"sv},

    // 2-byte partition [0x0080..0x07FF] and sign-bit seams 0x80, 0xFF
    {"two_byte_min_sign_seam_high", 0x0080U, "\xC2\x80"sv},
    {"uint8_max_seam", 0x00FFU, "\xC3\xBF"sv},
    {"two_byte_max", 0x07FFU, "\xDF\xBF"sv},

    // 3-byte lower BMP partition [0x0800..0xD7FF]
    {"three_byte_min", 0x0800U, "\xE0\xA0\x80"sv},
    {"bmp_euro_interior", 0x20ACU, "\xE2\x82\xAC"sv},
    {"pre_surrogate_max", 0xD7FFU, "\xED\x9F\xBF"sv},

    // UTF-16 surrogate partition [0xD800..0xDFFF] (never valid scalar UTF-8)
    {"high_surrogate_min", 0xD800U, ""sv},
    {"high_surrogate_max", 0xDBFFU, ""sv},
    {"low_surrogate_min", 0xDC00U, ""sv},
    {"low_surrogate_max", 0xDFFFU, ""sv},

    // 3-byte upper BMP partition [0xE000..0xFFFF]
    {"post_surrogate_min", 0xE000U, "\xEE\x80\x80"sv},
    {"bmp_max", 0xFFFFU, "\xEF\xBF\xBF"sv},

    // 4-byte supplementary partition [0x10000..0x10FFFF]
    {"four_byte_min", 0x10000U, "\xF0\x90\x80\x80"sv},
    {"supplementary_clef_interior", 0x1D11EU, "\xF0\x9D\x84\x9E"sv},
    {"unicode_max", 0x10FFFFU, "\xF4\x8F\xBF\xBF"sv},

    // Out-of-range invalid partition [0x110000..0xFFFFFFFF]
    {"above_unicode_min", 0x110000U, ""sv},
    {"uint32_max_signedness_seam", 0xFFFFFFFFU, ""sv},
};

constexpr std::size_t kExpectedUtf8CaseCount = 20;
static_assert(std::size(kUtf8Cases) == kExpectedUtf8CaseCount);

TEST(Utf8, EncodesCodePointPartitionsAndBoundaries) {
    ASSERT_EQ(std::size(kUtf8Cases), kExpectedUtf8CaseCount);
    for (const auto& tc : kUtf8Cases) {
        SCOPED_TRACE(tc.name);
        std::string out;
        pdf2md::append_utf8(out, tc.cp);
        EXPECT_EQ(std::string_view(out), tc.expected);
    }
}

TEST(Utf8, AppendsAndConvertsStringsAcrossInvalidCodePointSeams) {
    // Valid -> surrogate -> valid -> above-Unicode -> valid preserves prior buffer state.
    std::string acc = "prefix:";
    pdf2md::append_utf8(acc, U'Z');
    pdf2md::append_utf8(acc, 0xD800U);
    pdf2md::append_utf8(acc, 0x00E9U);
    pdf2md::append_utf8(acc, 0x110000U);
    pdf2md::append_utf8(acc, 0x1D11EU);
    EXPECT_EQ(acc, "prefix:Z\xC3\xA9\xF0\x9D\x84\x9E");

    EXPECT_EQ(pdf2md::to_utf8(std::u32string_view{}), "");

    const std::array<char32_t, 7> mixed = {
        U'h', 0x00E9U, 0xDC00U, 0x0000U, 0x20ACU, 0xFFFFFFFFU, 0x1D11EU,
    };
    const std::u32string_view view(mixed.data(), mixed.size());
    EXPECT_EQ(std::string_view(pdf2md::to_utf8(view)),
              "h\xC3\xA9\x00\xE2\x82\xAC\xF0\x9D\x84\x9E"sv);
}

constexpr std::size_t kMaxUtf16Bytes = 12;

struct Utf16LeCase {
    const char* name;
    std::array<unsigned char, kMaxUtf16Bytes> bytes;
    std::size_t byte_len;
    std::string_view expected;
};

constexpr Utf16LeCase kUtf16LeCases[] = {
    {"zero_bytes_non_null", {{0x41, 0x00}}, 0, ""sv},
    {"single_odd_byte_truncated", {{0x41}}, 1, ""sv},
    {"three_bytes_drops_trailing_odd_byte", {{0x41, 0x00, 0x42}}, 3, "A"sv},
    {"stops_at_embedded_nul_code_unit", {{0x41, 0x00, 0x00, 0x00, 0x42, 0x00}}, 6, "A"sv},
    {"zero_low_byte_with_nonzero_high_byte_is_not_nul",
     {{0x00, 0x01, 0x00, 0x00}},
     4,
     "\xC4\x80"sv},
    {"bmp_and_supplementary_clef_pair",
     {{0x41, 0x00, 0x34, 0xD8, 0x1E, 0xDD, 0x00, 0x00}},
     8,
     "A\xF0\x9D\x84\x9E"sv},
    {"surrogate_pair_min_u10000", {{0x00, 0xD8, 0x00, 0xDC}}, 4, "\xF0\x90\x80\x80"sv},
    {"surrogate_pair_max_u10ffff", {{0xFF, 0xDB, 0xFF, 0xDF}}, 4, "\xF4\x8F\xBF\xBF"sv},
    {"unpaired_high_surrogate_at_buffer_end", {{0x41, 0x00, 0x34, 0xD8}}, 4, "A"sv},
    {"unpaired_high_surrogate_before_bmp_preserves_bmp",
     {{0x34, 0xD8, 0x41, 0x00, 0x00, 0x00}},
     6,
     "A"sv},
    {"unpaired_high_surrogate_before_nul_does_not_skip_terminator",
     {{0x41, 0x00, 0x34, 0xD8, 0x00, 0x00, 0x42, 0x00}},
     8,
     "A"sv},
    {"consecutive_high_surrogates_pair_second_with_low",
     {{0x34, 0xD8, 0x34, 0xD8, 0x1E, 0xDD}},
     6,
     "\xF0\x9D\x84\x9E"sv},
    {"unpaired_low_surrogate_before_bmp", {{0x1E, 0xDD, 0x42, 0x00}}, 4, "B"sv},
    {"unpaired_low_surrogate_at_buffer_end", {{0x42, 0x00, 0x1E, 0xDD}}, 4, "B"sv},
    {"high_bit_bytes_0xff_and_0x80_without_sign_extension",
     {{0xFF, 0x00, 0x80, 0x80}},
     4,
     "\xC3\xBF\xE8\x82\x80"sv},
};

constexpr std::size_t kExpectedUtf16CaseCount = 15;
static_assert(std::size(kUtf16LeCases) == kExpectedUtf16CaseCount);

TEST(Utf16Le, DecodesLittleEndianUnitsSurrogateSeamsAndTerminators) {
    EXPECT_EQ(pdf2md::utf16le_to_utf8(nullptr, 0), "");

    ASSERT_EQ(std::size(kUtf16LeCases), kExpectedUtf16CaseCount);
    for (const auto& tc : kUtf16LeCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(std::string_view(pdf2md::utf16le_to_utf8(tc.bytes.data(), tc.byte_len)),
                  tc.expected);
    }
}

struct CpPredicateCase {
    const char* name;
    char32_t cp;
    bool expected;
};

constexpr CpPredicateCase kSpaceCases[] = {
    {"backspace_below_tab", 0x0008U, false},
    {"ascii_tab", 0x0009U, true},
    {"line_feed_not_horizontal_space", 0x000AU, false},
    {"carriage_return_not_horizontal_space", 0x000DU, false},
    {"unit_separator_below_space", 0x001FU, false},
    {"ascii_space", 0x0020U, true},
    {"exclamation_above_space", 0x0021U, false},
    {"control_below_nbsp", 0x009FU, false},
    {"no_break_space", 0x00A0U, true},
    {"inverted_exclamation_above_nbsp", 0x00A1U, false},
    {"soft_hyphen_not_space", 0x00ADU, false},
    {"below_ogham_space", 0x167FU, false},
    {"ogham_space_mark", 0x1680U, true},
    {"above_ogham_space", 0x1681U, false},
    {"below_en_quad_range", 0x1FFFU, false},
    {"en_quad_range_min", 0x2000U, true},
    {"four_per_em_space_interior", 0x2005U, true},
    {"hair_space_range_max", 0x200AU, true},
    {"zero_width_space_above_range", 0x200BU, false},
    {"below_narrow_nbsp", 0x202EU, false},
    {"narrow_nbsp", 0x202FU, true},
    {"above_narrow_nbsp", 0x2030U, false},
    {"below_medium_math_space", 0x205EU, false},
    {"medium_math_space", 0x205FU, true},
    {"word_joiner_above_math_space", 0x2060U, false},
    {"below_ideographic_space", 0x2FFFU, false},
    {"ideographic_space", 0x3000U, true},
    {"ideographic_comma_above_space", 0x3001U, false},
    {"above_unicode_max", 0x110000U, false},
};

constexpr std::size_t kExpectedSpaceCaseCount = 29;
static_assert(std::size(kSpaceCases) == kExpectedSpaceCaseCount);

TEST(SpaceClassification, RecognizesHorizontalWhitespaceBoundaries) {
    ASSERT_EQ(std::size(kSpaceCases), kExpectedSpaceCaseCount);
    for (const auto& tc : kSpaceCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_space_cp(tc.cp), tc.expected);
    }
}

constexpr CpPredicateCase kCjkCases[] = {
    // [0x2E80..0x9FFF] CJK Radicals .. Unified Ideographs
    {"below_cjk_radicals", 0x2E7FU, false},
    {"cjk_radicals_min", 0x2E80U, true},
    {"cjk_unified_interior", 0x4E2DU, true},
    {"cjk_unified_max", 0x9FFFU, true},
    {"yi_syllables_above_cjk_unified", 0xA000U, false},

    // [0xAC00..0xD7AF] Hangul Syllables
    {"below_hangul_syllables", 0xABFFU, false},
    {"hangul_syllables_min", 0xAC00U, true},
    {"hangul_syllables_max", 0xD7AFU, true},
    {"hangul_jamo_ext_b_above_syllables", 0xD7B0U, false},

    // [0xF900..0xFAFF] CJK Compatibility Ideographs
    {"below_cjk_compat", 0xF8FFU, false},
    {"cjk_compat_min", 0xF900U, true},
    {"cjk_compat_max", 0xFAFFU, true},
    {"latin_ff_ligature_above_cjk_compat", 0xFB00U, false},

    // [0xFF00..0xFFEF] Halfwidth and Fullwidth Forms
    {"bom_below_fullwidth", 0xFEFFU, false},
    {"fullwidth_min", 0xFF00U, true},
    {"fullwidth_max", 0xFFEFU, true},
    {"specials_above_fullwidth", 0xFFF0U, false},

    // [0x20000..0x2FA1F] CJK Unified Ideographs Ext B..F & Compat Supplement
    {"below_cjk_ext_b", 0x1FFFFU, false},
    {"cjk_ext_b_min", 0x20000U, true},
    {"cjk_compat_supp_max", 0x2FA1FU, true},
    {"above_cjk_compat_supp", 0x2FA20U, false},
};

constexpr std::size_t kExpectedCjkCaseCount = 21;
static_assert(std::size(kCjkCases) == kExpectedCjkCaseCount);

TEST(SpaceClassification, ClassifiesCjkRangeBoundaries) {
    ASSERT_EQ(std::size(kCjkCases), kExpectedCjkCaseCount);
    for (const auto& tc : kCjkCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_cjk_cp(tc.cp), tc.expected);
    }
}

struct AsciiRangeCase {
    const char* name;
    char32_t cp;
    bool digit;
    bool lower;
    bool upper;
    bool alpha;
};

constexpr AsciiRangeCase kAsciiCases[] = {
    {"slash_below_zero", U'/', false, false, false, false},
    {"digit_zero_min", U'0', true, false, false, false},
    {"digit_five_interior", U'5', true, false, false, false},
    {"digit_nine_max", U'9', true, false, false, false},
    {"colon_above_nine", U':', false, false, false, false},

    {"at_sign_below_upper_a", U'@', false, false, false, false},
    {"upper_a_min", U'A', false, false, true, true},
    {"upper_m_interior", U'M', false, false, true, true},
    {"upper_z_max", U'Z', false, false, true, true},
    {"left_bracket_above_upper_z", U'[', false, false, false, false},

    {"grave_below_lower_a", U'`', false, false, false, false},
    {"lower_a_min", U'a', false, true, false, true},
    {"lower_m_interior", U'm', false, true, false, true},
    {"lower_z_max", U'z', false, true, false, true},
    {"left_brace_above_lower_z", U'{', false, false, false, false},

    {"latin_accented_e_not_ascii", 0x00E9U, false, false, false, false},
    {"fullwidth_digit_zero_not_ascii", 0xFF10U, false, false, false, false},
    {"uint32_max_not_ascii", 0xFFFFFFFFU, false, false, false, false},
};

constexpr std::size_t kExpectedAsciiCaseCount = 18;
static_assert(std::size(kAsciiCases) == kExpectedAsciiCaseCount);

TEST(SpaceClassification, ClassifiesAsciiCharacterRanges) {
    ASSERT_EQ(std::size(kAsciiCases), kExpectedAsciiCaseCount);
    for (const auto& tc : kAsciiCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_ascii_digit(tc.cp), tc.digit);
        EXPECT_EQ(pdf2md::is_ascii_lower(tc.cp), tc.lower);
        EXPECT_EQ(pdf2md::is_ascii_upper(tc.cp), tc.upper);
        EXPECT_EQ(pdf2md::is_ascii_alpha(tc.cp), tc.alpha);
    }
}

constexpr CpPredicateCase kLineBreakCases[] = {
    {"horizontal_tab_below_lf", 0x0009U, false},
    {"line_feed_lf", 0x000AU, true},
    {"vertical_tab_above_lf", 0x000BU, false},
    {"form_feed_below_cr", 0x000CU, false},
    {"carriage_return_cr", 0x000DU, true},
    {"shift_out_above_cr", 0x000EU, false},
    {"ascii_space_not_line_break", 0x0020U, false},
    {"del_sign_seam_low", 0x007FU, false},
    {"pad_sign_seam_high", 0x0080U, false},
    {"index_below_nel", 0x0084U, false},
    {"next_line_nel", 0x0085U, true},
    {"start_selected_area_above_nel", 0x0086U, false},
    {"hyphenation_point_below_line_sep", 0x2027U, false},
    {"unicode_line_separator", 0x2028U, true},
    {"unicode_paragraph_separator", 0x2029U, true},
    {"left_to_right_embedding_above_para_sep", 0x202AU, false},
    {"above_unicode_max", 0x110000U, false},
};

constexpr std::size_t kExpectedLineBreakCaseCount = 17;
static_assert(std::size(kLineBreakCases) == kExpectedLineBreakCaseCount);

TEST(SpaceClassification, RecognizesLineBreakBoundariesAndSpaceDisjointness) {
    ASSERT_EQ(std::size(kLineBreakCases), kExpectedLineBreakCaseCount);
    for (const auto& tc : kLineBreakCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_line_break_cp(tc.cp), tc.expected);
        EXPECT_FALSE(pdf2md::is_line_break_cp(tc.cp) && pdf2md::is_space_cp(tc.cp));
    }
}

struct AsciiLowerCase {
    const char* name;
    char32_t cp;
    char expected;
};

constexpr AsciiLowerCase kAsciiLowerCases[] = {
    {"nul_preserved", 0x0000U, '\0'},
    {"digit_zero_unchanged", U'0', '0'},
    {"at_sign_below_upper_a", U'@', '@'},
    {"upper_a_min", U'A', 'a'},
    {"upper_m_interior", U'M', 'm'},
    {"upper_z_max", U'Z', 'z'},
    {"left_bracket_above_upper_z", U'[', '['},
    {"grave_below_lower_a", U'`', '`'},
    {"lower_a_unchanged", U'a', 'a'},
    {"lower_z_unchanged", U'z', 'z'},
    {"left_brace_above_lower_z", U'{', '{'},
    {"del_sign_seam_low", 0x007FU, '\x7F'},
    {"c1_sign_seam_high", 0x0080U, static_cast<char>(0x80U)},
    {"non_ascii_sharing_low_byte_with_upper_a_not_lowercased", 0x0141U, '\x41'},
};

constexpr std::size_t kExpectedAsciiLowerCaseCount = 14;
static_assert(std::size(kAsciiLowerCases) == kExpectedAsciiLowerCaseCount);

TEST(CharacterClassification, ConvertsAsciiToLowerAcrossCaseAndHighByteSeams) {
    ASSERT_EQ(std::size(kAsciiLowerCases), kExpectedAsciiLowerCaseCount);
    for (const auto& tc : kAsciiLowerCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::ascii_tolower(tc.cp), tc.expected);
    }
}

struct GreekAndLetterCase {
    const char* name;
    char32_t cp;
    bool expected_greek;
    bool expected_letter;
};

constexpr GreekAndLetterCase kGreekAndLetterCases[] = {
    {"at_sign_below_ascii_upper", U'@', false, false},
    {"ascii_upper_a", U'A', false, true},
    {"ascii_upper_z", U'Z', false, true},
    {"left_bracket_above_ascii_upper", U'[', false, false},
    {"grave_below_ascii_lower", U'`', false, false},
    {"ascii_lower_a", U'a', false, true},
    {"ascii_lower_z", U'z', false, true},
    {"left_brace_above_ascii_lower", U'{', false, false},
    {"ascii_digit_not_letter", U'5', false, false},

    {"below_greek_upper_alpha", 0x0390U, false, false},
    {"greek_upper_alpha_min", 0x0391U, true, true},
    {"greek_upper_pi_interior", 0x03A0U, true, true},
    {"greek_upper_omega_max", 0x03A9U, true, true},
    {"above_greek_upper_omega", 0x03AAU, false, false},

    {"below_greek_lower_alpha", 0x03B0U, false, false},
    {"greek_lower_alpha_min", 0x03B1U, true, true},
    {"greek_lower_mu_interior", 0x03BCU, true, true},
    {"greek_lower_omega_max", 0x03C9U, true, true},
    {"above_greek_lower_omega", 0x03CAU, false, false},

    {"below_theta_symbol", 0x03D0U, false, false},
    {"greek_theta_symbol", 0x03D1U, true, true},
    {"upsilon_hook_above_theta_symbol", 0x03D2U, false, false},
    {"below_phi_symbol", 0x03D4U, false, false},
    {"greek_phi_symbol", 0x03D5U, true, true},
    {"greek_pi_symbol", 0x03D6U, true, true},
    {"kai_symbol_above_pi_symbol", 0x03D7U, false, false},
    {"kappa_symbol_below_rho_symbol", 0x03F0U, false, false},
    {"greek_rho_symbol", 0x03F1U, true, true},
    {"lunate_sigma_above_rho_symbol", 0x03F2U, false, false},
    {"capital_theta_below_lunate_epsilon", 0x03F4U, false, false},
    {"greek_lunate_epsilon_symbol", 0x03F5U, true, true},
    {"reversed_lunate_epsilon_above", 0x03F6U, false, false},
    {"cjk_ideograph_not_ascii_or_greek_letter", 0x4E2DU, false, false},
};

constexpr std::size_t kExpectedGreekLetterCaseCount = 33;
static_assert(std::size(kGreekAndLetterCases) == kExpectedGreekLetterCaseCount);

TEST(CharacterClassification, ClassifiesGreekAndLetterRangeSeams) {
    ASSERT_EQ(std::size(kGreekAndLetterCases), kExpectedGreekLetterCaseCount);
    for (const auto& tc : kGreekAndLetterCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_greek_cp(tc.cp), tc.expected_greek);
        EXPECT_EQ(pdf2md::is_letter_cp(tc.cp), tc.expected_letter);
    }
}

struct BulletAndDashCase {
    const char* name;
    char32_t cp;
    bool expected_bullet;
    bool expected_dashy;
};

constexpr BulletAndDashCase kBulletAndDashCases[] = {
    {"right_paren_below_asterisk", U')', false, false},
    {"ascii_asterisk_bullet", U'*', true, false},
    {"plus_sign_above_asterisk", U'+', false, false},
    {"comma_below_hyphen", U',', false, false},
    {"ascii_hyphen_dashy", U'-', false, true},
    {"period_above_hyphen", U'.', false, false},
    {"soft_hyphen_not_dashy", pdf2md::kSoftHyphenCp, false, false},

    {"pilcrow_below_middle_dot", 0x00B6U, false, false},
    {"middle_dot_bullet", 0x00B7U, true, false},
    {"cedilla_above_middle_dot", 0x00B8U, false, false},

    {"unicode_hyphen_not_dashy", pdf2md::kUnicodeHyphenCp, false, false},
    {"figure_dash_below_en_dash", 0x2012U, false, false},
    {"en_dash_min", 0x2013U, false, true},
    {"em_dash_max", 0x2014U, false, true},
    {"horizontal_bar_above_em_dash", 0x2015U, false, false},

    {"double_dagger_below_bullet_pair", 0x2021U, false, false},
    {"round_bullet_min", 0x2022U, true, false},
    {"triangular_bullet_max", 0x2023U, true, false},
    {"one_dot_leader_above_bullet_pair", 0x2024U, false, false},

    {"hyphen_bullet_isolated", 0x2043U, true, false},
    {"fraction_slash_above_hyphen_bullet", 0x2044U, false, false},
    {"bullet_operator", 0x2219U, true, false},
    {"square_root_above_bullet_operator", 0x221AU, false, false},

    {"black_square_bullet", 0x25A0U, true, false},
    {"white_square_interior_hole_not_bullet", 0x25A1U, false, false},
    {"black_small_square_bullet", 0x25AAU, true, false},
    {"white_circle_bullet", 0x25CBU, true, false},
    {"dotted_circle_interior_hole_not_bullet", 0x25CCU, false, false},
    {"black_circle_bullet", 0x25CFU, true, false},
    {"white_bullet", 0x25E6U, true, false},
    {"ballot_box_not_plain_bullet", 0x2610U, false, false},
};

constexpr std::size_t kExpectedBulletDashCaseCount = 31;
static_assert(std::size(kBulletAndDashCases) == kExpectedBulletDashCaseCount);

TEST(CharacterClassification, ClassifiesBulletAndDashRangeSeams) {
    ASSERT_EQ(std::size(kBulletAndDashCases), kExpectedBulletDashCaseCount);
    for (const auto& tc : kBulletAndDashCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_EQ(pdf2md::is_bullet_cp(tc.cp), tc.expected_bullet);
        EXPECT_EQ(pdf2md::is_dashy_cp(tc.cp), tc.expected_dashy);
    }
}

TEST(Utf8Path, RoundTripsUnicodePathsAndNormalizesGenericSlashes) {
    const std::filesystem::path empty_path = pdf2md::path_from_utf8("");
    EXPECT_EQ(empty_path, std::filesystem::path{});
    EXPECT_EQ(pdf2md::path_to_utf8(empty_path), "");
    EXPECT_EQ(pdf2md::path_to_utf8_generic(empty_path), "");

    constexpr std::string_view kUnicodeRel =
        "docs/\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xCE\xB1_\xF0\x9F\x9A\x80/page 1.md"sv;
    const std::filesystem::path decoded = pdf2md::path_from_utf8(kUnicodeRel);
    EXPECT_EQ(decoded.filename().u32string(), U"page 1.md");
    EXPECT_EQ(decoded.parent_path().filename().u32string(),
              U"\u65E5\u672C\u8A9E_\u03B1_\U0001F680");
    EXPECT_EQ(pdf2md::path_to_utf8_generic(decoded), std::string(kUnicodeRel));
    EXPECT_EQ(pdf2md::path_to_utf8(pdf2md::path_from_utf8(
                  "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xF0\x9F\x93\x84.pdf")),
              "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xF0\x9F\x93\x84.pdf");

    constexpr std::string_view kBackslashInput =
        "assets\\\xE5\x9B\xB3_\xF0\x9F\x93\x84\\fig_01.png"sv;
    const std::filesystem::path win_style = pdf2md::path_from_utf8(kBackslashInput);
    EXPECT_EQ(pdf2md::path_to_utf8_generic(win_style),
              "assets/\xE5\x9B\xB3_\xF0\x9F\x93\x84/fig_01.png");
}

}  // namespace
