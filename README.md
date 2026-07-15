# pdf2md

A PDF → Markdown converter written in C++20. It extracts text with per-character
geometry and font metadata via **PDFium**, reconstructs the document structure with
layout analysis, and emits clean GitHub-flavored Markdown.

## Features

- **Headings** inferred from font-size tiers (`#`–`######`), plus bold body-size
  lines as the lowest tier
- **Paragraphs** rebuilt from wrapped lines, with hyphenation repair
  (`hyphen-`/`ation` → `hyphenation`, including PDFium's dehyphenation markers)
  and merging of paragraphs split across page boundaries
- **Bold / italic / inline code** from font weight, style flags and font names
- **Bullet and numbered lists** with nesting from indentation
- **Fenced code blocks** for monospace text, with indentation reconstructed from
  glyph positions
- **Tables** from aligned column layouts (best effort, `--no-tables` to disable)
- **Inline math** — super/subscripts and math symbols (Greek, operators) rebuilt
  as LaTeX in `$…$` (e.g. `d_k`, `QK^T`, `W_i^Q`, `β_1`), which GitHub/MathJax
  renders; detected from glyph size and baseline offset. Stacked display
  fractions and radicals are best-effort (no true `\frac` reconstruction)
- **Multi-column pages** read in correct order via recursive XY-cut segmentation
- **Repeated headers/footers and page numbers** detected across pages and stripped
- **Image extraction** to PNG with `--images DIR` (rendered via PDFium, linked
  in the markdown, deduplicated by content hash)
- **Encrypted PDFs** via `--password`, metadata front matter via `--front-matter`
- Full **Unicode** pipeline (UTF-8 output, surrogate pairs, ligature expansion,
  non-ASCII paths on Windows)

## Building

Requires CMake ≥ 3.23, Conan 2, and a C++20 compiler (tested with MSVC 2022).
The `build.py` helper runs the whole flow — Conan install, CMake configure, and
build — picking the right preset for your OS:

```
python build.py            # configure + build (Release)
python build.py --test     # build, then run the ctest suite
```

Run `python build.py -h` for options (`--profile`, `--preset`, `-j`, …). The
binary lands in `build/Release/pdf2md` (`.exe` on Windows). Dependencies
(pdfium, CLI11, stb, gtest, libharu) come from ConanCenter; prebuilt binaries
exist for common platforms.

## Usage

```
pdf2md input.pdf                     # writes input.md next to the PDF
pdf2md input.pdf -o out.md           # explicit output ('-' for stdout)
pdf2md input.pdf --pages 1-3,7       # convert selected pages (1-based)
pdf2md input.pdf --images assets     # extract images as PNG into assets/
pdf2md input.pdf --front-matter      # YAML front matter from PDF metadata
pdf2md input.pdf --page-breaks       # '---' between pages
```

| Flag | Effect |
| --- | --- |
| `--password PW` | open an encrypted PDF |
| `--keep-headers` | keep repeated page headers/footers |
| `--no-headings` | do not infer headings from font sizes |
| `--no-tables` | do not infer tables from aligned columns |
| `--keep-rotated` | keep rotated text (watermarks are skipped by default) |
| `-q, --quiet` | suppress the summary line |

Exit codes: `0` success, `1` bad arguments, `2` PDF load/parse error, `3` output I/O error.

## Tests

Google Test suite in `tests/`; the test fixtures are PDFs generated at test run
time by libharu (`tests/fixture_pdfs.cpp`), so no binary fixtures are checked in.

```
python build.py --test
```

Each run leaves its inputs and results in `build/test_output/` for inspection:
`fixtures/` holds the generated PDFs plus the converted `.md` for each (and
`.<variant>.md` for option variants like `basic.keep-headers.md`), and
`corpus/` holds one `.md` per downloaded real-world PDF.

The `Examples.GenerateGalleryWithCli` test (`tests/examples_export.cpp`) builds
a browsable gallery under `build/examples/`: it runs the built `pdf2md`
executable over every fixture PDF and every downloaded corpus PDF, leaving each
`<name>.pdf` beside the `<name>.md` the tool produces. Because it drives the
real CLI, it doubles as an end-to-end smoke test of the shipped binary.

### Real-world corpus tests

A second layer of smoke tests runs against well-known public PDFs (the pdf.js
tracemonkey two-column paper, the *Attention Is All You Need* arXiv paper,
pdfminer samples, tabula table PDFs with CJK text, a PDF 2.0 example). Pass
`--with-corpus` to fetch them first; the `Corpus.*` tests then stop skipping:

```
python build.py --with-corpus --test
```

## Debugging

`PDF2MD_DEBUG_RAW=1` dumps PDFium's raw character stream (code point + position)
to stderr — useful when a document's layout confuses the heuristics.

## Architecture

The converter is three subsystems behind one facade; the free functions
`extract_pdf` / `analyze_layout` / `write_markdown` remain the stable seams the
tests drive directly.

```
src/
  converter.*             Facade: ConvertOptions -> extract -> analyze -> write
  pdf_extractor.*         Adapter over PDFium's C API (RAII handle owners; the
                          library-init Meyers singleton): pages -> characters
                          (code point, baseline, bbox, size, style) + metadata,
                          images, ruling lines
  layout_analyzer.cpp     analyze_layout: the per-page assembly orchestrator
    layout_lines.cpp        XY-cut region split -> baseline-clustered lines
    layout_math.cpp         inline math -> LaTeX styled runs
    layout_blocks.cpp       lists, display equations, line stream -> blocks
    layout_tables_text.cpp  aligned-column (text-geometry) table detection
    layout_tables_ruled.cpp ruled-lattice (bordered grid) table recovery
    layout_passes.cpp       doc-level cleanup as a DocumentPass pipeline
                            (Strategy with a Template Method gate, assembled
                            by a Factory Method; the order is a contract)
    layout_internal.h       the pdf2md::detail seam shared by these modules
  markdown_writer.*       MarkdownWriter (per-conversion state, BlockKind
                          dispatch) + MarkdownAssembler (Builder for the
                          output document); blocks -> GFM
  image_extractor.*       PDFium bitmaps -> PNG (stb_image_write)
  main.cpp                CLI (CLI11), wmain on Windows for Unicode arguments;
                          delegates the pipeline to the Converter facade
tests/                    Google Test suite + libharu fixture generators
```

Design patterns are applied only where they carry weight: Facade
(`Converter`), Adapter + RAII (the PDFium boundary), a Meyers Singleton for
library init (deliberately the only singleton), Strategy / Template Method /
Factory Method (the document-pass pipeline), and Builder (markdown assembly).
Deliberately rejected as poor fits here: Visitor or a polymorphic Block
hierarchy (Block is a value struct that passes re-tag in place), State for the
block classifier, and Command / Observer / Interpreter / Flyweight (no
undo/queueing, no event graph, no grammar, no interning pressure).

Layout reconstruction is heuristic by nature; the defaults are tuned for
common single- and two-column documents. Scanned (image-only) PDFs produce no
text — OCR is out of scope.
