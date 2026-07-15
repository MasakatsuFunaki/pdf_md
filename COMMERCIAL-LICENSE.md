# Commercial License

`pdf2md` is dual-licensed. You may use it under **either** of the following, at
your choice:

1. The **GNU Affero General Public License v3.0** (see [`LICENSE`](LICENSE)) — free of
   charge, but with the obligations described below.
2. A **commercial license** purchased from the copyright holder — no copyleft
   obligations, no requirement to publish your source code.

## When the AGPL is enough

The AGPL costs nothing and permits commercial use, sale, and modification. It is
the right choice if you are comfortable with its central condition: **anything
you build on pdf2md must itself be released under the AGPL, with complete
corresponding source code available to your users** — including users who only
reach it over a network.

That last part is what distinguishes the AGPL from the ordinary GPL. Section 13
extends the source-code obligation to software offered as a hosted service: if
you run a modified pdf2md behind a web app, SaaS product, or internal API that
other people use, those users are entitled to its source.

## When you need the commercial license

Buy a commercial license if any of the following apply:

- You want to **link pdf2md into proprietary software** and keep that software
  closed-source.
- You want to **offer pdf2md as part of a hosted or SaaS product** without
  publishing your source code.
- You want to **redistribute pdf2md inside a product** whose license terms are
  incompatible with the AGPL.
- Your organisation's policy prohibits AGPL-licensed dependencies (many do).
- You need warranty terms, indemnification, or support commitments — the AGPL
  explicitly provides none.

The commercial license removes the copyleft requirement entirely. Terms and
pricing are negotiated per engagement and depend on your use case, distribution
model, and support needs.

## Contact

To enquire about commercial licensing, contact:

**Masakatsu Funaki** — <pularaulviclean@gmail.com>

Please include a short description of your product, how you intend to integrate
pdf2md, and whether you plan to distribute it or run it as a service.

## Contributions

Contributions are welcome under the AGPL. Because this project is dual-licensed,
the copyright holder must be able to relicense contributed code commercially —
so by submitting a pull request you agree that your contribution may be
distributed under both the AGPL and the commercial license. For substantial
contributions a signed Contributor License Agreement may be requested.

## Third-party dependencies

pdf2md links against third-party libraries under their own permissive licenses,
all of which are compatible with both halves of this dual license:

| Dependency | License |
| --- | --- |
| PDFium | BSD-3-Clause |
| CLI11 | BSD-3-Clause |
| stb | MIT / public domain |
| GoogleTest (tests only) | BSD-3-Clause |
| libharu (tests only) | ZLIB |

These retain their original licenses regardless of which pdf2md license you
choose; their terms are not affected by this document.
