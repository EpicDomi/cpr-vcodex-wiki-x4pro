//! Writes the XWA test archives the firmware's reader is checked against,
//! and for each a dump of what xwa-tool's own reader finds in it.
//!
//! The firmware test (test/xwa) prints the same dump from the C++ reader and
//! compares it line by line, so both readers must agree on every article,
//! search, fold, image and named place.
//!
//! Usage: cargo run --release -- <output dir>

use std::fmt::Write as _;
use std::fs;
use std::io::Write as _;
use std::path::Path;

use xwa_format::content::{
    Anchor, Article, Block, Cell, Formula, Infobox, InfoboxRow, Inline, ListKind, MathItem, PathOp,
    Place, Style, Table, anchor_hash, decode_article, decode_header,
};
use xwa_format::math_glyphs::{GlyphOp, GlyphTable, Outline};
use xwa_format::reader::Archive;
use xwa_format::records::{article_flags, image_encoding, image_kind};
use xwa_format::writer::{ArchiveWriter, ImageInfo, WriterOptions};
use xwa_format::{DAYS_IN_YEAR, Profile, la2};

/// A small deterministic generator, so the archives never change by accident.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as u32
    }
}

const WORDS: &[&str] = &[
    "the", "archive", "reader", "draws", "pages", "quickly", "while", "small", "devices", "keep",
    "their", "memory", "for", "text", "and", "pictures", "of", "every", "article", "it", "opens",
    "with", "links", "to", "others", "in", "a", "wiki", "written", "by", "many", "people",
    "über", "naïve", "café", "Ελληνικά", "кириллица", "ˈaɪnʃtaɪn", "→", "≤",
];

fn sentence(rng: &mut Rng, words: usize) -> String {
    let mut s = String::new();
    for i in 0..words {
        if i > 0 {
            s.push(' ');
        }
        let w = WORDS[(rng.next() as usize) % WORDS.len()];
        if i == 0 {
            let mut c = w.chars();
            s.extend(c.next().unwrap().to_uppercase());
            s.push_str(c.as_str());
        } else {
            s.push_str(w);
        }
    }
    s.push('.');
    s
}

fn t(s: &str) -> Inline {
    Inline::Text(s.to_string())
}

fn styled(style: Style, content: Vec<Inline>) -> Inline {
    Inline::Styled(style, content)
}

fn link(article: u32, section: u32, text: &str) -> Inline {
    Inline::Link {
        article,
        place: Place::Section(section),
        content: vec![t(text)],
    }
}

fn link_anchor(article: u32, name: &str, text: &str) -> Inline {
    Inline::Link {
        article,
        place: Place::Anchor(anchor_hash(name)),
        content: vec![t(text)],
    }
}

fn para(content: Vec<Inline>) -> Block {
    Block::Para(content)
}

fn heading(level: u8, text: &str) -> Block {
    Block::Heading {
        level,
        content: vec![t(text)],
    }
}

fn inline_formula() -> Formula {
    Formula {
        display: false,
        width: 1800,
        ascent: 750,
        descent: 250,
        items: vec![
            MathItem::Glyph {
                glyph: 0,
                scale: 0,
                dx: 0,
                dy: 0,
            },
            MathItem::Glyph {
                glyph: 1,
                scale: 1,
                dx: 600,
                dy: 350,
            },
            MathItem::Rule {
                dx: -600,
                dy: -450,
                w: 1200,
                h: 40,
            },
            MathItem::GlyphXY {
                glyph: 2,
                sx: 300,
                sy: 200,
                dx: 1250,
                dy: 100,
            },
        ],
        fallback_image: None,
    }
}

fn block_formula() -> Formula {
    Formula {
        display: true,
        width: 2400,
        ascent: 900,
        descent: 300,
        items: vec![
            MathItem::Path {
                dx: 0,
                dy: -300,
                ops: vec![
                    PathOp::Move(0, 0),
                    PathOp::Line(400, 0),
                    PathOp::Quad(500, 100, 400, 200),
                    PathOp::Cubic(300, 300, 100, 300, 0, 200),
                    PathOp::Close,
                ],
            },
            MathItem::Rule {
                dx: 500,
                dy: 300,
                w: 800,
                h: 40,
            },
            MathItem::GlyphXY {
                glyph: 2,
                sx: 320,
                sy: 200,
                dx: 900,
                dy: 100,
            },
            MathItem::Glyph {
                glyph: 0,
                scale: 2,
                dx: -100,
                dy: -500,
            },
        ],
        fallback_image: Some(1),
    }
}

fn glyph_table() -> GlyphTable {
    let square = Outline {
        bbox: [0, 0, 500, 500],
        ops: vec![
            GlyphOp::Move(0, 0),
            GlyphOp::Line(500, 0),
            GlyphOp::Line(500, 500),
            GlyphOp::Line(0, 500),
            GlyphOp::Close,
        ],
    };
    let drop = Outline {
        bbox: [0, -100, 400, 600],
        ops: vec![
            GlyphOp::Move(200, -100),
            GlyphOp::Quad(400, 100, 300, 400),
            GlyphOp::Line(200, 600),
            GlyphOp::Line(100, 400),
            GlyphOp::Quad(0, 100, 200, -100),
            GlyphOp::Close,
        ],
    };
    // A ring: two contours, the inner one wound the other way, so nonzero
    // filling leaves a hole.
    let ring = Outline {
        bbox: [0, 0, 600, 600],
        ops: vec![
            GlyphOp::Move(300, 0),
            GlyphOp::Cubic(466, 0, 600, 134, 600, 300),
            GlyphOp::Cubic(600, 466, 466, 600, 300, 600),
            GlyphOp::Cubic(134, 600, 0, 466, 0, 300),
            GlyphOp::Cubic(0, 134, 134, 0, 300, 0),
            GlyphOp::Close,
            GlyphOp::Move(300, 150),
            GlyphOp::Cubic(217, 150, 150, 217, 150, 300),
            GlyphOp::Cubic(150, 383, 217, 450, 300, 450),
            GlyphOp::Cubic(383, 450, 450, 383, 450, 300),
            GlyphOp::Cubic(450, 217, 383, 150, 300, 150),
            GlyphOp::Close,
        ],
    };
    GlyphTable {
        upem: 1000,
        scales: vec![256, 179, 128],
        glyphs: vec![square, drop, ring],
    }
}

/// The rich article: every block and inline kind the format has.
fn alpha(rng: &mut Rng) -> Article {
    let mut body = vec![
        Block::InfoboxRef,
        para(vec![
            t("Alpha is a "),
            styled(Style::Bold, vec![t("test")]),
            t(" article with "),
            styled(
                Style::Italic,
                vec![t("italic "), styled(Style::Bold, vec![t("bold italic")])],
            ),
            t(", H"),
            styled(Style::Subscript, vec![t("2")]),
            t("O, E = mc"),
            styled(Style::Superscript, vec![t("2")]),
            t(", "),
            styled(Style::Monospace, vec![t("code()")]),
            t(" and "),
            styled(Style::SmallCaps, vec![t("Small Caps")]),
            t(". See "),
            link(1, 2, "Beta's career"),
            t(" and "),
            link_anchor(1, "History_2", "Beta's later history"),
            t("."),
            Inline::Break,
            t("A flag "),
            Inline::Icon {
                image: 2,
                height: 15,
            },
            t(" in a line, a formula "),
            Inline::Math(inline_formula()),
            t(" and a note"),
            Inline::NoteRef(1),
            t("."),
        ]),
        heading(2, "Lists"),
        Block::List {
            kind: ListKind::Bullet,
            items: vec![
                vec![para(vec![t("One")])],
                vec![
                    para(vec![t("Two")]),
                    Block::List {
                        kind: ListKind::Number,
                        items: vec![
                            vec![para(vec![t("Two point one")])],
                            vec![para(vec![t("Two point two")])],
                        ],
                    },
                ],
                vec![para(vec![t("Three")])],
            ],
        },
        Block::List {
            kind: ListKind::Definition,
            items: vec![
                vec![para(vec![styled(Style::Bold, vec![t("Term")])])],
                vec![para(vec![t("What the term means.")])],
            ],
        },
        heading(2, "Quotes and code"),
        Block::Quote(vec![para(vec![t("A quoted paragraph.")])]),
        Block::Pre("fn main() {\n        println!(\"hi\");\n}".to_string()),
        Block::Rule,
        heading(3, "Pictures"),
        Block::Figure {
            image: 0,
            caption: vec![t("A "), styled(Style::Italic, vec![t("photo")]), t(" caption")],
        },
        Block::Figure {
            image: 1,
            caption: vec![t("Line art")],
        },
        Block::Gallery(vec![
            (0, vec![t("First")]),
            (1, vec![t("Second")]),
            (3, vec![t("Third")]),
        ]),
        heading(2, "Math"),
        Block::MathBlock(block_formula()),
        heading(2, "Long text"),
    ];
    // Enough text that the article runs through several 4 KB chunks.
    for _ in 0..12 {
        let mut run = Vec::new();
        for i in 0..8 {
            if i > 0 {
                run.push(t(" "));
            }
            run.push(t(&sentence(rng, 18)));
        }
        // Tidy runs: one Text per sentence group, no double spaces.
        let text: String = run
            .iter()
            .map(|i| match i {
                Inline::Text(s) => s.as_str(),
                _ => "",
            })
            .collect();
        body.push(para(vec![t(&text)]));
    }
    body.push(heading(2, "Tables"));
    body.push(para(vec![styled(Style::Bold, vec![t("A caption")])]));
    body.push(Block::TableRef(0));
    body.push(Block::TableRef(1));
    let cell = |header: bool, rowspan: u32, colspan: u32, content: Vec<Inline>| Cell {
        header,
        rowspan,
        colspan,
        content,
    };
    let table0 = Table {
        rows: 3,
        cols: 3,
        flags: 0,
        cells: vec![
            cell(true, 1, 1, vec![t("H1")]),
            cell(true, 1, 1, vec![t("H2")]),
            cell(true, 1, 1, vec![t("H3")]),
            cell(false, 2, 1, vec![t("Spans two rows")]),
            cell(false, 1, 1, vec![t("a")]),
            cell(false, 1, 1, vec![link(1, 0, "b")]),
            cell(false, 1, 2, vec![t("c and d")]),
        ],
    };
    let mut wide = Vec::new();
    for r in 0..2 {
        for c in 0..8 {
            wide.push(cell(
                r == 0,
                1,
                1,
                vec![t(&format!("Row {r} column {c} with words"))],
            ));
        }
    }
    let table1 = Table {
        rows: 2,
        cols: 8,
        flags: 0,
        cells: wide,
    };
    Article {
        title: "Alpha".into(),
        body,
        tables: vec![table0, table1],
        infobox: Some(Infobox {
            rows: vec![
                InfoboxRow::Header(vec![t("Alpha")]),
                InfoboxRow::Image {
                    image: 0,
                    caption: vec![t("A photo")],
                },
                InfoboxRow::Field {
                    label: vec![t("Born")],
                    value: vec![t("1 January 1900")],
                },
                InfoboxRow::Field {
                    label: vec![t("Known for")],
                    value: vec![link(1, 0, "Beta")],
                },
                InfoboxRow::Full(vec![t("A row as wide as the box")]),
            ],
        }),
        anchors: vec![Anchor {
            hash: anchor_hash("Old_name"),
            section: 1,
        }],
    }
}

fn beta(rng: &mut Rng) -> Article {
    let mut body = vec![para(vec![t(&sentence(rng, 12))])];
    for (level, name) in [
        (2, "Early life"),
        (2, "Career"),
        (2, "History"),
        (3, "Details"),
        (2, "History"),
    ] {
        body.push(heading(level, name));
        body.push(para(vec![t(&sentence(rng, 20))]));
    }
    Article {
        title: "Beta".into(),
        body,
        tables: Vec::new(),
        infobox: None,
        anchors: vec![Anchor {
            hash: anchor_hash("History_2"),
            section: 5,
        }],
    }
}

fn simple(title: &str, text: &str) -> Article {
    Article {
        title: title.into(),
        body: vec![para(vec![t(text)])],
        ..Default::default()
    }
}

fn gray_jpeg(w: u16, h: u16) -> Vec<u8> {
    let mut pixels = Vec::with_capacity(w as usize * h as usize);
    for y in 0..h as u32 {
        for x in 0..w as u32 {
            let ring = (((x as i32 - w as i32 / 2).pow(2) + (y as i32 - h as i32 / 2).pow(2)) as f32)
                .sqrt() as u32;
            pixels.push(((x * 255 / w as u32) ^ (ring * 8 & 0x40)) as u8);
        }
    }
    let mut out = Vec::new();
    jpeg_encoder::Encoder::new(&mut out, 80)
        .encode(&pixels, w, h, jpeg_encoder::ColorType::Luma)
        .unwrap();
    out
}

/// LA2 (spec 9.2) as xwa-tool writes it: deflate with a 4 KB window for the
/// compact profile, zstd for the full one.
fn la2_image(w: u16, h: u16, bpp: u8, seed: u32, profile: Profile) -> Vec<u8> {
    let (wu, hu) = (w as usize, h as usize);
    let stride = (wu * bpp as usize).div_ceil(8);
    let mut packed = vec![0u8; stride * hu];
    for y in 0..hu {
        for x in 0..wu {
            let v = (((x * 3 + y * 5) as u32 / (4 + seed % 5) + (x * y) as u32 % 7) % 4) as u8;
            let v = if bpp == 1 { v / 2 } else { v };
            let bit = x * bpp as usize;
            packed[y * stride + bit / 8] |= v << (8 - bpp as usize - bit % 8);
        }
    }
    let (codec, body) = match profile {
        Profile::Compact => {
            let mut c =
                flate2::Compress::new_with_window_bits(flate2::Compression::best(), false, 12);
            let mut out = Vec::with_capacity(packed.len() + 64);
            loop {
                let input = &packed[c.total_in() as usize..];
                let status = c
                    .compress_vec(input, &mut out, flate2::FlushCompress::Finish)
                    .unwrap();
                if status == flate2::Status::StreamEnd {
                    break;
                }
                out.reserve(out.capacity().max(1024));
            }
            (la2::codec::DEFLATE, out)
        }
        Profile::Full => (la2::codec::ZSTD, zstd::bulk::compress(&packed, 15).unwrap()),
    };
    let mut out = Vec::new();
    out.extend_from_slice(&w.to_le_bytes());
    out.extend_from_slice(&h.to_le_bytes());
    out.extend_from_slice(&[bpp, codec]);
    out.extend_from_slice(&body);
    out
}

const FILLERS: u32 = 400;

fn write_archive(path: &Path, profile: Profile) {
    let mut opts = WriterOptions::new(profile);
    // Small chunks so that articles cross chunk boundaries in a small file.
    opts.chunk_size = 4096;
    opts.threads = 2;
    opts.dict_sample_bytes = 64 << 10;
    opts.dict_size = 8 << 10;
    let _ = fs::remove_file(path);
    let mut w = ArchiveWriter::create(path, opts).unwrap();
    for (k, v) in [
        ("title", "Firmware test wiki"),
        ("description", "Every block, inline and section the firmware reads"),
        ("language", "eng"),
        ("source.name", "xwa_testgen"),
        ("source.date", "2026-11-17"),
        ("attribution", "Test text, public domain"),
        ("license", "Unlicense"),
    ] {
        w.set_meta(k, v);
    }
    let mut rng = Rng(42);
    let mut articles: Vec<(Article, u16, u8)> = vec![
        (alpha(&mut rng), 0, 250),
        (beta(&mut rng), 0, 180),
        (simple("Ça va", "Ça va bien, merci."), 0, 40),
        (simple("Straße", "Eine Straße in der Stadt."), 0, 35),
        (simple("कि", "हिन्दी का एक शब्द।"), 0, 20),
        (simple("क", "एक अक्षर।"), 0, 21),
        (simple("が", "ひらがなの一文字。"), 0, 22),
        (simple("か", "もう一つの文字。"), 0, 23),
        (simple("한국", "대한민국의 이름입니다."), 0, 60),
        (simple("كتاب", "هذا كتاب عربي، and some English في الوسط."), 0, 45),
        (simple("שלום", "שלום עולם, hello world."), 0, 44),
        (simple("January 1", "The first day of the year."), 0, 90),
        (simple("February 29", "A leap day."), 0, 70),
        (simple("December 31", "The last day of the year."), 0, 91),
        (
            simple("Mercury", "Mercury may refer to several things."),
            article_flags::DISAMBIGUATION,
            100,
        ),
        (
            simple("List of tests", "A list."),
            article_flags::LIST,
            8,
        ),
        (simple("Einstein", "A physicist."), 0, 200),
        (simple("Eindhoven", "A city."), 0, 120),
        (simple("Eine kleine Nachtmusik", "A serenade."), 0, 50),
        (simple("Einar", "A name."), 0, 30),
        (simple("Ein", "A word."), 0, 10),
        (simple("Ein Abend", "An evening."), 0, 5),
    ];
    for i in 0..FILLERS {
        articles.push((
            simple(&format!("Filler {i:04}"), &format!("Filler {i} is number {i}.")),
            0,
            ((i * 37) % 251) as u8,
        ));
    }
    for (id, (article, flags, _)) in articles.iter().enumerate() {
        w.add_article(id as u32, article, *flags).unwrap();
    }
    for (id, (_, _, pop)) in articles.iter().enumerate() {
        w.set_popularity(id as u32, *pop).unwrap();
    }
    for (title, target, anchor) in [
        ("Alfa", 0, None),
        ("ALPHA", 0, None),
        ("Beta history", 1, Some(anchor_hash("History_2"))),
        ("Beta's career", 1, Some(anchor_hash("Career"))),
        ("Albert Einstein", 16, None),
        ("Einstein (physicist)", 16, None),
        ("Strasse", 3, None),
    ] {
        w.add_redirect(title, target, anchor).unwrap();
    }
    let images: Vec<(ImageInfo, Vec<u8>)> = vec![
        (
            ImageInfo {
                encoding: image_encoding::JPEG,
                width: 120,
                height: 80,
                kind: image_kind::PHOTO,
                flags: 0,
            },
            gray_jpeg(120, 80),
        ),
        (
            ImageInfo {
                encoding: image_encoding::LINE_ART,
                width: 64,
                height: 40,
                kind: image_kind::LINE_ART,
                flags: 0,
            },
            la2_image(64, 40, 2, 1, profile),
        ),
        (
            ImageInfo {
                encoding: image_encoding::LINE_ART,
                width: 24,
                height: 16,
                kind: image_kind::ICON,
                flags: 0,
            },
            la2_image(24, 16, 1, 2, profile),
        ),
        (
            ImageInfo {
                encoding: image_encoding::LINE_ART,
                width: 300,
                height: 200,
                kind: image_kind::LINE_ART,
                flags: 1,
            },
            la2_image(300, 200, 2, 3, profile),
        ),
        (
            ImageInfo {
                encoding: image_encoding::JPEG,
                width: 600,
                height: 400,
                kind: image_kind::PHOTO,
                flags: 0,
            },
            gray_jpeg(600, 400),
        ),
    ];
    for (id, (info, data)) in images.into_iter().enumerate() {
        w.add_image(id as u32, info, data).unwrap();
    }
    w.set_math_glyphs(xwa_format::math_glyphs::encode(&glyph_table()).unwrap());
    let mut days = [None; DAYS_IN_YEAR];
    days[0] = Some(11);
    days[59] = Some(12);
    days[365] = Some(13);
    w.set_days(&days);
    w.finish().unwrap();
}

// ------------------------------------------------------------------ dumping

fn esc(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for c in s.chars() {
        match c {
            '\\' => o.push_str("\\\\"),
            '"' => o.push_str("\\\""),
            '\n' => o.push_str("\\n"),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn formula(f: &Formula) -> String {
    let mut o = format!(
        "disp={} w={} asc={} desc={} fb={}",
        u8::from(f.display),
        f.width,
        f.ascent,
        f.descent,
        f.fallback_image.map_or("-".to_string(), |i| i.to_string())
    );
    for item in &f.items {
        o.push(' ');
        match item {
            MathItem::Glyph {
                glyph,
                scale,
                dx,
                dy,
            } => write!(o, "G{glyph}s{scale}({dx},{dy})").unwrap(),
            MathItem::GlyphXY {
                glyph,
                sx,
                sy,
                dx,
                dy,
            } => write!(o, "X{glyph}[{sx},{sy}]({dx},{dy})").unwrap(),
            MathItem::Rule { dx, dy, w, h } => write!(o, "R{w}x{h}({dx},{dy})").unwrap(),
            MathItem::Path { dx, dy, ops } => {
                write!(o, "P({dx},{dy})").unwrap();
                for op in ops {
                    match op {
                        PathOp::Move(x, y) => write!(o, "M{x},{y}").unwrap(),
                        PathOp::Line(x, y) => write!(o, "L{x},{y}").unwrap(),
                        PathOp::Quad(a, b, x, y) => write!(o, "Q{a},{b},{x},{y}").unwrap(),
                        PathOp::Cubic(a, b, c, d, x, y) => {
                            write!(o, "C{a},{b},{c},{d},{x},{y}").unwrap()
                        }
                        PathOp::Close => o.push('Z'),
                    }
                }
            }
        }
    }
    o
}

fn inlines(list: &[Inline]) -> String {
    let mut o = String::new();
    for i in list {
        match i {
            Inline::Text(s) => o.push_str(&esc(s)),
            Inline::Styled(style, c) => {
                write!(o, "<s{}>{}</s>", *style as u8, inlines(c)).unwrap();
            }
            Inline::Link {
                article,
                place,
                content,
            } => {
                match place {
                    Place::Section(s) => write!(o, "<a{article}#{s}>").unwrap(),
                    Place::Anchor(h) => write!(o, "<a{article}@{h:08x}>").unwrap(),
                }
                o.push_str(&inlines(content));
                o.push_str("</a>");
            }
            Inline::Math(f) => write!(o, "<m {}>", formula(f)).unwrap(),
            Inline::Icon { image, height } => write!(o, "<i{image}h{height}>").unwrap(),
            Inline::Break => o.push_str("<br>"),
            Inline::NoteRef(n) => write!(o, "<n{n}>").unwrap(),
        }
    }
    o
}

fn blocks(out: &mut String, list: &[Block], depth: usize) {
    let pad = "  ".repeat(depth);
    for b in list {
        match b {
            Block::Para(c) => writeln!(out, "{pad}PARA {}", inlines(c)).unwrap(),
            Block::Heading { level, content } => {
                writeln!(out, "{pad}H{level} {}", inlines(content)).unwrap()
            }
            Block::List { kind, items } => {
                let k = match kind {
                    ListKind::Bullet => 0,
                    ListKind::Number => 1,
                    ListKind::Definition => 2,
                };
                writeln!(out, "{pad}LIST {k}").unwrap();
                for item in items {
                    writeln!(out, "{pad}  ITEM").unwrap();
                    blocks(out, item, depth + 2);
                }
                writeln!(out, "{pad}ENDLIST").unwrap();
            }
            Block::Quote(inner) => {
                writeln!(out, "{pad}QUOTE").unwrap();
                blocks(out, inner, depth + 1);
                writeln!(out, "{pad}ENDQUOTE").unwrap();
            }
            Block::Pre(s) => writeln!(out, "{pad}PRE {}", esc(s)).unwrap(),
            Block::Rule => writeln!(out, "{pad}RULE").unwrap(),
            Block::Figure { image, caption } => {
                writeln!(out, "{pad}FIG {image} {}", inlines(caption)).unwrap()
            }
            Block::MathBlock(f) => writeln!(out, "{pad}MATH {}", formula(f)).unwrap(),
            Block::TableRef(i) => writeln!(out, "{pad}TABLEREF {i}").unwrap(),
            Block::InfoboxRef => writeln!(out, "{pad}INFOBOXREF").unwrap(),
            Block::Gallery(items) => {
                writeln!(out, "{pad}GALLERY {}", items.len()).unwrap();
                for (image, caption) in items {
                    writeln!(out, "{pad}  GITEM {image} {}", inlines(caption)).unwrap();
                }
            }
        }
    }
}

fn hit(o: &mut String, kind: &str, h: &xwa_format::title_index::Hit) {
    writeln!(
        o,
        "  {kind} {} a={} f={} p={} h={}",
        esc(&h.display),
        h.article,
        h.flags,
        h.popularity,
        h.anchor.map_or("-".to_string(), |a| format!("{a:08x}"))
    )
    .unwrap();
}

fn dump(path: &Path) -> String {
    let ar = Archive::open(path).unwrap();
    let mut o = String::new();
    let sb = ar.superblock();
    writeln!(
        o,
        "SB profile={} codec={} chunk_bits={} req={:x} opt={:x} articles={} chunks={} images={}",
        sb.profile.code(),
        sb.text_codec.code(),
        ar.chunk_size().trailing_zeros(),
        sb.required_features,
        sb.optional_features,
        ar.article_count(),
        ar.chunk_count(),
        ar.image_count()
    )
    .unwrap();
    for (k, v) in ar.meta() {
        writeln!(o, "META {k}={v}").unwrap();
    }
    for id in 0..ar.article_count() {
        let rec = ar.article_record(id).unwrap();
        let stream = ar.article_stream(id).unwrap();
        let (header, _) = decode_header(&stream).unwrap();
        let article = decode_article(&stream).unwrap();
        writeln!(
            o,
            "ARTICLE {id} {} chunks={},{},{} flags={} pop={} title={}",
            esc(&header.title),
            rec.first_chunk,
            rec.offset_in_chunk,
            rec.chunk_count,
            rec.flags,
            rec.popularity,
            esc(&ar.title(id).unwrap())
        )
        .unwrap();
        writeln!(o, "BODYSIZE {}", header.body_size).unwrap();
        for e in &header.toc {
            writeln!(o, "TOC {} {} {}", e.level, e.pos, esc(&e.text)).unwrap();
        }
        let pos: Vec<String> = header.table_pos.iter().map(|p| p.to_string()).collect();
        writeln!(o, "TABLES {}", pos.join(" ")).unwrap();
        writeln!(
            o,
            "INFOBOX {}",
            header.infobox_pos.map_or("-".to_string(), |p| p.to_string())
        )
        .unwrap();
        for a in &header.anchors {
            writeln!(o, "ANCHOR {:08x} {}", a.hash, a.section).unwrap();
        }
        writeln!(o, "BODY").unwrap();
        blocks(&mut o, &article.body, 1);
        for (i, table) in article.tables.iter().enumerate() {
            writeln!(o, "TABLE {i} {}x{} f={}", table.rows, table.cols, table.flags).unwrap();
            for c in &table.cells {
                writeln!(
                    o,
                    "  CELL h={} {}x{} {}",
                    u8::from(c.header),
                    c.rowspan,
                    c.colspan,
                    inlines(&c.content)
                )
                .unwrap();
            }
        }
        if let Some(ib) = &article.infobox {
            writeln!(o, "INFOBOXROWS {}", ib.rows.len()).unwrap();
            for row in &ib.rows {
                match row {
                    InfoboxRow::Header(c) => writeln!(o, "  IHEAD {}", inlines(c)),
                    InfoboxRow::Image { image, caption } => {
                        writeln!(o, "  IIMG {image} {}", inlines(caption))
                    }
                    InfoboxRow::Field { label, value } => {
                        writeln!(o, "  IFIELD {} | {}", inlines(label), inlines(value))
                    }
                    InfoboxRow::Full(c) => writeln!(o, "  IFULL {}", inlines(c)),
                }
                .unwrap();
            }
        }
        writeln!(o, "END").unwrap();
    }
    let queries = [
        "", "a", "al", "alpha", "ALPHA", "Alfa", "ça", "ca", "strasse", "STRASSE", "straße",
        "STRAẞE", "कि", "क", "が", "か", "한", "한국", "كتاب", "كِتَاب", "שָׁלוֹם", "ein", "eins",
        "e", "f", "filler 01", "Filler_0399", "zzz", "beta h", "beta's", "januar", "mercury",
        "  einstein  ", "ÉINSTEIN", "ﬁller", "ｆｉｌｌｅｒ",
    ];
    for q in queries {
        let key = ar.search_key(q);
        let hex: String = key.bytes().map(|b| format!("{b:02x}")).collect();
        writeln!(o, "QUERY {} key={hex}", esc(q)).unwrap();
        for h in ar.search(q, 12).unwrap() {
            hit(&mut o, "IDX", &h);
        }
        for h in ar.search_popular(q, 8).unwrap() {
            hit(&mut o, "POP", &h);
        }
    }
    if let Some(days) = ar.days().unwrap() {
        for (i, d) in days.iter().enumerate() {
            if let Some(a) = d {
                writeln!(o, "DAY {i} {a}").unwrap();
            }
        }
    }
    for id in 0..ar.image_count() {
        let (rec, data) = ar.image(id).unwrap();
        writeln!(
            o,
            "IMAGE {id} enc={} {}x{} kind={} flags={} size={} crc={:08x}",
            rec.encoding,
            rec.width,
            rec.height,
            rec.kind,
            rec.flags,
            rec.size,
            crc32fast::hash(&data)
        )
        .unwrap();
        if rec.encoding == image_encoding::LINE_ART {
            let img = la2::decode(&data).unwrap();
            writeln!(
                o,
                "LA2 {id} bpp={} codec={} levels_crc={:08x}",
                img.header.bpp,
                img.header.codec,
                crc32fast::hash(&img.levels)
            )
            .unwrap();
        }
    }
    if let Some(g) = ar.math_glyphs().unwrap() {
        let scales: Vec<String> = g.scales.iter().map(|s| s.to_string()).collect();
        writeln!(
            o,
            "GLYPHS upem={} count={} scales={}",
            g.upem,
            g.glyphs.len(),
            scales.join(",")
        )
        .unwrap();
        for (i, gl) in g.glyphs.iter().enumerate() {
            writeln!(
                o,
                "GLYPH {i} bbox={},{},{},{} ops={}",
                gl.bbox[0],
                gl.bbox[1],
                gl.bbox[2],
                gl.bbox[3],
                gl.ops.len()
            )
            .unwrap();
        }
    }
    for (id, name) in [
        (1, "History_2"),
        (1, "Career"),
        (1, "Early_life"),
        (1, "History"),
        (1, "Details"),
        (0, "Old_name"),
        (0, "Lists"),
        (0, "Quotes_and_code"),
        (1, "Nope"),
    ] {
        let h = anchor_hash(name);
        let place = ar.find_place(id, h).unwrap();
        writeln!(
            o,
            "PLACE {id} {h:08x} {}",
            place.map_or("-".to_string(), |p| p.to_string())
        )
        .unwrap();
    }
    o
}

fn main() {
    let dir = std::env::args().nth(1).expect("usage: xwa-testgen <output dir>");
    let dir = Path::new(&dir);
    fs::create_dir_all(dir).unwrap();
    for (name, profile) in [("rich-full", Profile::Full), ("rich-compact", Profile::Compact)] {
        let path = dir.join(format!("{name}.xwa"));
        write_archive(&path, profile);
        let text = dump(&path);
        let mut f = fs::File::create(dir.join(format!("{name}.expected.txt"))).unwrap();
        f.write_all(text.as_bytes()).unwrap();
        let report = Archive::open(&path)
            .unwrap()
            .verify(&mut |_, _, _| {})
            .unwrap();
        assert!(report.ok(), "{name}: {report:?}");
        println!(
            "{name}: {} bytes, {} dump lines",
            fs::metadata(&path).unwrap().len(),
            text.lines().count()
        );
    }
}
