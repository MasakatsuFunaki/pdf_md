#include <string>

#include <gtest/gtest.h>

#include "utf8.h"

namespace {

std::string encode(char32_t cp) {
    std::string out;
    pdf2md::append_utf8(out, cp);
    return out;
}

TEST(Utf8, EncodesAllWidths) {
    EXPECT_EQ(encode(U'A'), "A");
    EXPECT_EQ(encode(0x00E9), "\xC3\xA9");          // é
    EXPECT_EQ(encode(0x20AC), "\xE2\x82\xAC");      // €
    EXPECT_EQ(encode(0x1D11E), "\xF0\x9D\x84\x9E"); // 𝄞
}

TEST(Utf8, DropsInvalidCodePoints) {
    EXPECT_EQ(encode(0x110000), "");
}

TEST(Utf8, RejectsSurrogateCodePoints) {
    EXPECT_EQ(encode(0xD800), "");
    EXPECT_EQ(encode(0xDBFF), "");
    EXPECT_EQ(encode(0xDC00), "");
    EXPECT_EQ(encode(0xDFFF), "");
}

TEST(Utf8, ToUtf8ConvertsString) {
    EXPECT_EQ(pdf2md::to_utf8(U"héllo"), "h\xC3\xA9llo");
}

TEST(Utf16Le, DecodesBasicAndSurrogatePairs) {
    // "A" + U+1D11E (D834 DD1E) in UTF-16LE.
    const unsigned char bytes[] = {0x41, 0x00, 0x34, 0xD8, 0x1E, 0xDD, 0x00, 0x00};
    EXPECT_EQ(pdf2md::utf16le_to_utf8(bytes, sizeof(bytes)), "A\xF0\x9D\x84\x9E");
}

TEST(Utf16Le, SkipsUnpairedSurrogates) {
    const unsigned char bytes[] = {0x34, 0xD8, 0x41, 0x00, 0x00, 0x00};
    EXPECT_EQ(pdf2md::utf16le_to_utf8(bytes, sizeof(bytes)), "A");
}

TEST(Utf16Le, StopsAtNul) {
    const unsigned char bytes[] = {0x41, 0x00, 0x00, 0x00, 0x42, 0x00};
    EXPECT_EQ(pdf2md::utf16le_to_utf8(bytes, sizeof(bytes)), "A");
}

TEST(SpaceClassification, RecognizesUnicodeSpaces) {
    EXPECT_TRUE(pdf2md::is_space_cp(U' '));
    EXPECT_TRUE(pdf2md::is_space_cp(0x00A0));
    EXPECT_TRUE(pdf2md::is_space_cp(0x2009));
    EXPECT_FALSE(pdf2md::is_space_cp(U'x'));
    EXPECT_FALSE(pdf2md::is_space_cp(0x00AD));  // soft hyphen is not a space
}

}  // namespace
