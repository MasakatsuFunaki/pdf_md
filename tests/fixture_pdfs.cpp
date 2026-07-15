#include "fixture_pdfs.h"

#include <cstdio>
#include <stdexcept>
#include <string>

#include <hpdf.h>

namespace pdf2md::testfx {

namespace {

void error_handler(HPDF_STATUS error_no, HPDF_STATUS detail_no, void*) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "libharu error 0x%04X detail %u",
                  static_cast<unsigned>(error_no), static_cast<unsigned>(detail_no));
    throw std::runtime_error(buf);
}

struct Fonts {
    HPDF_Font regular, bold, italic, mono;
};

Fonts load_fonts(HPDF_Doc pdf) {
    return Fonts{
        HPDF_GetFont(pdf, "Helvetica", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Helvetica-Bold", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Helvetica-Oblique", "WinAnsiEncoding"),
        HPDF_GetFont(pdf, "Courier", "WinAnsiEncoding"),
    };
}

// RAII so a throwing error handler cannot leak the document.
struct Doc {
    HPDF_Doc pdf;
    Doc() : pdf(HPDF_New(error_handler, nullptr)) {
        if (!pdf) throw std::runtime_error("HPDF_New failed");
    }
    ~Doc() { HPDF_Free(pdf); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
};

void text_at(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char* text) {
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, font, size);
    HPDF_Page_TextOut(page, x, y, text);
    HPDF_Page_EndText(page);
}

float text_width(HPDF_Page page, HPDF_Font font, float size, const char* text) {
    HPDF_Page_SetFontAndSize(page, font, size);
    return HPDF_Page_TextWidth(page, text);
}

HPDF_Page add_letter_page(HPDF_Doc pdf) {
    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
    return page;
}

void header_footer(HPDF_Page page, const Fonts& fonts, int page_no) {
    text_at(page, fonts.regular, 9, 72, 758, "PDF2MD Fixture Suite");
    std::string footer = "Page " + std::to_string(page_no);
    text_at(page, fonts.regular, 9, 290, 36, footer.c_str());
}

}  // namespace

void make_basic(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.pdf;
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Basic Fixture Document");
    HPDF_SetInfoAttr(pdf, HPDF_INFO_AUTHOR, "pdf2md tests");
    Fonts fonts = load_fonts(pdf);

    // ---- page 1
    HPDF_Page p1 = add_letter_page(pdf);
    header_footer(p1, fonts, 1);

    text_at(p1, fonts.bold, 24, 72, 700, "Sample Document Title");
    text_at(p1, fonts.bold, 16, 72, 660, "First Section");

    float y = 634;
    text_at(p1, fonts.regular, 11, 72, y, "This is the first paragraph of the sample document. It spans multiple");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "lines and should be joined into a single markdown paragraph without");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "unwanted line breaks.");

    // Paragraph with inline bold and italic runs.
    y -= 30;
    float x = 72;
    const char* seg1 = "The converter must detect ";
    text_at(p1, fonts.regular, 11, x, y, seg1);
    x += text_width(p1, fonts.regular, 11, seg1);
    const char* seg2 = "bold text";
    text_at(p1, fonts.bold, 11, x, y, seg2);
    x += text_width(p1, fonts.bold, 11, seg2);
    const char* seg3 = " and also ";
    text_at(p1, fonts.regular, 11, x, y, seg3);
    x += text_width(p1, fonts.regular, 11, seg3);
    const char* seg4 = "italic text";
    text_at(p1, fonts.italic, 11, x, y, seg4);
    x += text_width(p1, fonts.italic, 11, seg4);
    text_at(p1, fonts.regular, 11, x, y, " inside a sentence.");

    text_at(p1, fonts.bold, 16, 72, y - 40, "Lists");
    y -= 64;
    text_at(p1, fonts.regular, 11, 72, y, "\225 First bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "\225 Second bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 100, y, "\225 Nested bullet item");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "\225 Third bullet item");
    y -= 22;
    text_at(p1, fonts.regular, 11, 72, y, "1. Alpha step");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "2. Beta step");
    y -= 15;
    text_at(p1, fonts.regular, 11, 72, y, "3. Gamma step");

    text_at(p1, fonts.bold, 16, 72, y - 40, "Code Example");
    y -= 64;
    text_at(p1, fonts.mono, 10, 72, y, "int main() {");
    y -= 13;
    text_at(p1, fonts.mono, 10, 72, y, "    return 42;");
    y -= 13;
    text_at(p1, fonts.mono, 10, 72, y, "}");

    // ---- page 2
    HPDF_Page p2 = add_letter_page(pdf);
    header_footer(p2, fonts, 2);
    text_at(p2, fonts.bold, 16, 72, 700, "Hyphenation");
    text_at(p2, fonts.regular, 11, 72, 674, "This paragraph exercises the hyphen-");
    text_at(p2, fonts.regular, 11, 72, 659, "ation repair logic across wrapped lines of the same para-");
    text_at(p2, fonts.regular, 11, 72, 644, "graph in the converter.");

    text_at(p2, fonts.bold, 16, 72, 600, "Bold Pseudo Heading Section");
    text_at(p2, fonts.regular, 11, 72, 574, "A paragraph that continues onto the next page must be merged into a");
    text_at(p2, fonts.regular, 11, 72, 559, "single paragraph even though it is split across the page");

    // ---- page 3
    HPDF_Page p3 = add_letter_page(pdf);
    header_footer(p3, fonts, 3);
    text_at(p3, fonts.regular, 11, 72, 700, "boundary, which is exactly what this final sentence verifies.");
    text_at(p3, fonts.regular, 11, 72, 660, "Final page content for the header stripping threshold.");

    HPDF_SaveToFile(pdf, out.string().c_str());
}

void make_twocol(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.pdf;
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Two Column Fixture");
    Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.bold, 18, 72, 720, "Two Column Layout Test");

    const char* left[] = {
        "Left column sentence one flows",
        "across several short lines and",
        "must be read completely before",
        "any right column text appears.",
    };
    const char* right[] = {
        "Right column sentence one also",
        "flows across several lines and",
        "must appear after the whole of",
        "the left column in the output.",
    };
    float y = 680;
    for (const char* line : left) {
        text_at(page, fonts.regular, 11, 72, y, line);
        y -= 15;
    }
    y = 680;
    for (const char* line : right) {
        text_at(page, fonts.regular, 11, 340, y, line);
        y -= 15;
    }

    HPDF_SaveToFile(pdf, out.string().c_str());
}

void make_table(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.pdf;
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Table Fixture");
    Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.bold, 18, 72, 720, "Inventory Report");

    const float col_x[3] = {72, 250, 420};
    const char* rows[4][3] = {
        {"Name", "Quantity", "Price"},
        {"Apples", "12", "1.50"},
        {"Bananas", "7", "0.75"},
        {"Cherries", "31", "4.20"},
    };
    float y = 680;
    for (int r = 0; r < 4; ++r) {
        HPDF_Font f = (r == 0) ? fonts.bold : fonts.regular;
        for (int c = 0; c < 3; ++c) text_at(page, f, 11, col_x[c], y, rows[r][c]);
        y -= 16;
    }

    text_at(page, fonts.regular, 11, 72, y - 24,
            "A regular paragraph after the table should not be absorbed into it.");

    HPDF_SaveToFile(pdf, out.string().c_str());
}

void make_image(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.pdf;
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Image Fixture");
    Fonts fonts = load_fonts(pdf);

    HPDF_Page page = add_letter_page(pdf);
    text_at(page, fonts.regular, 11, 72, 700, "A paragraph above the picture.");

    // 8x8 RGB checkerboard as raw pixel data.
    HPDF_BYTE pixels[8 * 8 * 3];
    for (int yy = 0; yy < 8; ++yy) {
        for (int xx = 0; xx < 8; ++xx) {
            HPDF_BYTE v = ((xx / 2 + yy / 2) % 2) ? 220 : 40;
            HPDF_BYTE* px = pixels + (yy * 8 + xx) * 3;
            px[0] = v;
            px[1] = static_cast<HPDF_BYTE>(255 - v);
            px[2] = 128;
        }
    }
    HPDF_Image img =
        HPDF_LoadRawImageFromMem(pdf, pixels, 8, 8, HPDF_CS_DEVICE_RGB, 8);
    HPDF_Page_DrawImage(page, img, 72, 560, 96, 96);

    text_at(page, fonts.regular, 11, 72, 520, "A paragraph below the picture.");

    HPDF_SaveToFile(pdf, out.string().c_str());
}

void make_runheads(const std::filesystem::path& out) {
    Doc doc;
    HPDF_Doc pdf = doc.pdf;
    HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, "Running Heads Fixture");
    Fonts fonts = load_fonts(pdf);

    const char* bodies[9] = {
        "Opening paragraph about alpha topics on the first page.",
        "Second page discusses beta material in its own words.",
        "Third page continues with gamma related observations.",
        "Fourth page rounds out the delta considerations nicely.",
        "Fifth page presents epsilon data in tabular form below.",
        "Sixth page presents zeta data in tabular form below.",
        "Seventh page presents eta data in tabular form below.",
        "Eighth page presents theta data in tabular form below.",
        "Ninth page closes the fixture with plain iota prose.",
    };
    const char* countries[4][3] = {
        {"France", "Paris", "68"},
        {"Japan", "Tokyo", "125"},
        {"Brazil", "Brasilia", "216"},
        {"Kenya", "Nairobi", "55"},
    };
    for (int p = 0; p < 9; ++p) {
        HPDF_Page page = add_letter_page(pdf);
        if (p < 4) {
            // Chapter-scoped running head: same top-margin spot on only 4 of 9
            // pages -- under the document-wide repeat bar, caught only by the
            // position-anchored pass.
            text_at(page, fonts.regular, 9, 72, 758, "GAMMA CHAPTER 7");
        } else if (p < 8) {
            // Borderless table whose header row repeats at a fixed top-margin
            // position with a data row right beneath: real content, not chrome.
            text_at(page, fonts.bold, 9, 72, 758, "Country");
            text_at(page, fonts.bold, 9, 200, 758, "Capital");
            text_at(page, fonts.bold, 9, 330, 758, "Population");
            text_at(page, fonts.regular, 9, 72, 744, countries[p - 4][0]);
            text_at(page, fonts.regular, 9, 200, 744, countries[p - 4][1]);
            text_at(page, fonts.regular, 9, 330, 744, countries[p - 4][2]);
        }
        text_at(page, fonts.regular, 11, 72, 400, bodies[p]);
    }
    HPDF_SaveToFile(pdf, out.string().c_str());
}

std::filesystem::path generate_all(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    make_basic(dir / "basic.pdf");
    make_twocol(dir / "twocol.pdf");
    make_table(dir / "table.pdf");
    make_image(dir / "image.pdf");
    make_runheads(dir / "runheads.pdf");
    return dir;
}

}  // namespace pdf2md::testfx
