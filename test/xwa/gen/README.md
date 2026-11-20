# XWA test archive generator

Writes the archives `test/xwa` reads (`../data/rich-full.xwa` and
`../data/rich-compact.xwa`) with xwa-tool's own writer, and next to each a
dump of how xwa-tool's own reader reads it (`*.expected.txt`). The C++ test
(`XwaReaderTest`) reads the same archives through `lib/Xwa`, prints the
same dump and compares it line by line, so the firmware's reader is checked
against the reference implementation on every block, inline, table,
infobox, formula, image, search and anchor case the format has.

The archives hold 422 articles: one with every block and inline kind
(including both kinds of links, an infobox, two tables, formulas with every
item kind, figures, a gallery and icons), titles in several scripts for
search folding (Latin with accents, ß/ẞ, Devanagari, Japanese kana, Hangul,
Arabic and Hebrew with marks), DAYS entries (including February 29), a
disambiguation page, redirects with and without anchors, and 400 filler
articles so the title index has several leaf and inner blocks. FONT and its
three styled sections come from the Noto Sans files in
`lib/EpdFont/builtinFonts/source/NotoSans`; FONT_ITALIC leaves out φ and ℝ
so that synthesised italic (slanted, and upright) is tested beside real
italic.

Only the C++ test runs in CI; the generated files are committed. Run the
generator again only when xwa-tool's format changes (update `rev` in
`Cargo.toml` to the xwa-tool commit that defines it):

```
cd test/xwa/gen
cargo build --release
target/release/xwa-testgen ../data
```

It needs network access to fetch xwa-tool from GitHub. The generator checks
both archives with xwa-tool's verifier before it finishes.
