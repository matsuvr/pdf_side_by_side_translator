use hayro::{
    PixmapSettings, RenderCache, RenderSettings,
    hayro_interpret::{self as pdf, font::GlyphRun, hayro_cmap::BfString, util::RectExt},
    hayro_syntax::{DecryptionError, LoadPdfError, Pdf, page::Page},
    kurbo::{Affine, BezPath, Point, Rect, Shape},
    vello_cpu::color::palette::css::WHITE,
};
use std::{path::Path, sync::Arc};
use unicode_normalization::UnicodeNormalization;

pub const SOURCE_LIMIT: usize = 256 * 1024;
pub enum OpenError {
    Password,
    Message(String),
}

pub struct Document(Pdf, pdf::InterpreterSettings);
pub struct PageImage {
    pub png: Arc<[u8]>,
    #[cfg(any(test, windows))]
    pub width: u32,
    #[cfg(any(test, windows))]
    pub height: u32,
    #[cfg(any(test, windows))]
    pub rgba: Vec<u8>,
}

impl Document {
    pub fn open(path: &Path, password: &str) -> Result<Self, OpenError> {
        // Deny concurrent writers on Windows so even a multi-GB sparse file
        // can be paged directly. On Unix an owned snapshot survives truncation.
        #[cfg(windows)]
        let data = {
            use std::os::windows::fs::OpenOptionsExt;
            let f = std::fs::OpenOptions::new()
                .read(true)
                .share_mode(1)
                .open(path)
                .map_err(|e| OpenError::Message(format!("PDFを開けません。{e}")))?;
            Arc::new(
                unsafe { memmap2::Mmap::map(&f) }.map_err(|e| OpenError::Message(e.to_string()))?,
            )
        };
        #[cfg(not(windows))]
        let data = std::fs::read(path).map_err(|e| OpenError::Message(e.to_string()))?;
        let pdf = Pdf::new_with_password(data, password).map_err(|e| match e {
            LoadPdfError::Decryption(DecryptionError::PasswordProtected) => OpenError::Password,
            _ => OpenError::Message("このPDFを読み込めません。".into()),
        })?;
        if pdf.pages().is_empty() {
            return Err(OpenError::Message("PDFにページがありません。".into()));
        }
        Ok(Self(pdf, crate::fonts::settings()))
    }
    pub fn count(&self) -> usize {
        self.0.pages().len()
    }
    fn page(&self, index: usize) -> Result<&Page<'_>, String> {
        self.0
            .pages()
            .get(index)
            .ok_or_else(|| "ページが見つかりません。".into())
    }
    pub fn image(&self, index: usize) -> Result<PageImage, String> {
        let page = self.page(index)?;
        let (w, h) = page.render_dimensions();
        if !w.is_finite() || !h.is_finite() || w <= 0. || h <= 0. {
            return Err("ページの寸法を取得できません。".into());
        }
        let scale = (2048.0 / w.max(h)).min(3.0);
        if w * scale < 1.0 || h * scale < 1.0 {
            return Err("ページの縦横比を表示できません。".into());
        }
        let pixels = hayro::render(
            page,
            &RenderCache::new(),
            &self.1,
            &RenderSettings::default(),
            &PixmapSettings {
                x_scale: scale,
                y_scale: scale,
                bg_color: WHITE,
            },
        );
        let (width, height) = (u32::from(pixels.width()), u32::from(pixels.height()));
        let rgba = pixels.data_as_u8_slice();
        let mut png = Vec::new();
        {
            let mut enc = png::Encoder::new(&mut png, width, height);
            enc.set_color(png::ColorType::Rgba);
            enc.set_depth(png::BitDepth::Eight);
            enc.set_compression(png::Compression::Fast);
            enc.write_header()
                .and_then(|mut w| w.write_image_data(rgba))
                .map_err(|e| e.to_string())?;
        }
        Ok(PageImage {
            png: png.into(),
            #[cfg(any(test, windows))]
            width,
            #[cfg(any(test, windows))]
            height,
            #[cfg(any(test, windows))]
            rgba: rgba.to_vec(),
        })
    }
    pub fn text(&self, index: usize) -> Result<String, String> {
        let page = self.page(index)?;
        let crop = page.intersected_crop_box().to_kurbo().abs();
        let cache = pdf::InterpreterCache::new();
        let mut ctx =
            pdf::Context::new(Affine::IDENTITY, crop, &cache, page.xref(), self.1.clone());
        let mut device = TextDevice {
            crop,
            clips: Vec::new(),
            glyphs: Vec::new(),
            over_limit: false,
            marked: Vec::new(),
            replacement_bytes: 0,
        };
        pdf::interpret_page(page, &mut ctx, &mut device);
        if device.over_limit {
            return Err("本文が長すぎるため、このページの翻訳を省略しました。".into());
        }
        Ok(reading_order(device.glyphs))
    }
}

struct Glyph {
    x: f64,
    y: f64,
    end: f64,
    size: f64,
    text: String,
}
struct TextDevice {
    crop: Rect,
    clips: Vec<Rect>,
    glyphs: Vec<Glyph>,
    over_limit: bool,
    marked: Vec<Option<(usize, String)>>,
    replacement_bytes: usize,
}
impl<'a> pdf::Device<'a> for TextDevice {
    fn begin_marked_content(
        &mut self,
        _: &[u8],
        properties: Option<&hayro::hayro_syntax::object::Dict<'_>>,
    ) {
        let replacement = properties
            .and_then(|dict| dict.get::<hayro::hayro_syntax::object::String<'_>>(b"ActualText"))
            .and_then(|s| {
                let bytes = s.as_bytes();
                self.replacement_bytes = self.replacement_bytes.saturating_add(bytes.len());
                if self.replacement_bytes > SOURCE_LIMIT * 2 {
                    self.over_limit = true;
                    return None;
                }
                if let Some(be) = bytes.strip_prefix(&[0xfe, 0xff]) {
                    let (pairs, remainder) = be.as_chunks::<2>();
                    if !remainder.is_empty() {
                        return None;
                    }
                    return char::decode_utf16(pairs.iter().copied().map(u16::from_be_bytes))
                        .collect::<Result<String, _>>()
                        .ok();
                }
                if let Some(utf8) = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]) {
                    return std::str::from_utf8(utf8).ok().map(str::to_owned);
                }
                // PDFDocEncoding (ISO 32000, Annex D) uses Latin-1 outside
                // these slots. PDF 2.0 UTF-8 strings carry a BOM.
                const ACCENTS: [char; 8] = ['˘', 'ˇ', 'ˆ', '˙', '˝', '˛', '˚', '˜'];
                const SPECIAL: [char; 32] = [
                    '•', '†', '‡', '…', '—', '–', 'ƒ', '⁄', '‹', '›', '−', '‰', '„', '“', '”', '‘',
                    '’', '‚', '™', 'ﬁ', 'ﬂ', 'Ł', 'Œ', 'Š', 'Ÿ', 'Ž', 'ı', 'ł', 'œ', 'š', 'ž', '�',
                ];
                Some(
                    bytes
                        .iter()
                        .map(|&b| match b {
                            0x18..=0x1f => ACCENTS[usize::from(b - 0x18)],
                            0x80..=0x9f => SPECIAL[usize::from(b - 0x80)],
                            0xa0 => '€',
                            0x7f | 0xad => '�',
                            _ => char::from(b),
                        })
                        .collect(),
                )
            });
        self.marked
            .push(replacement.map(|text| (self.glyphs.len(), text)));
    }
    fn end_marked_content(&mut self) {
        if let Some(Some((start, text))) = self.marked.pop()
            && start < self.glyphs.len()
        {
            let end = self.glyphs[start..]
                .iter()
                .map(|g| g.end)
                .fold(f64::NEG_INFINITY, f64::max);
            let x = self.glyphs[start..]
                .iter()
                .map(|g| g.x)
                .fold(f64::INFINITY, f64::min);
            self.glyphs.truncate(start + 1);
            self.glyphs[start].x = x;
            self.glyphs[start].end = end;
            self.glyphs[start].text = text;
        }
    }
    fn draw_glyph_run(
        &mut self,
        run: &GlyphRun<'_, 'a>,
        props: pdf::DrawProps<'a>,
        _: &pdf::DrawMode,
    ) {
        for glyph in run.glyphs() {
            if self.glyphs.len() >= 65536 {
                self.over_limit = true;
                break;
            }
            let m = props.transform * glyph.transform();
            let Point { x, y } = m * Point::ORIGIN;
            let clip = self.clips.last().unwrap_or(&self.crop);
            if !clip.contains(Point::new(x, y)) {
                continue;
            }
            let [a, b, _, _, _, _] = m.as_coeffs();
            let advance = match &**glyph {
                pdf::font::Glyph::Outline(g) => g.advance_width(),
                pdf::font::Glyph::Type3(g) => g.advance_width(),
            }
            .unwrap_or(500.);
            let end = x + a * f64::from(advance);
            let text = match glyph.as_unicode() {
                Some(BfString::Char(c)) => c.to_string(),
                Some(BfString::String(s)) => s,
                None => "�".into(),
            };
            self.glyphs.push(Glyph {
                x,
                y,
                end,
                size: (a.hypot(b) * 1000.).clamp(1., 300.),
                text,
            });
        }
    }
    fn draw_path(&mut self, _: &BezPath, _: pdf::DrawProps<'a>, _: &pdf::DrawMode) {}
    fn draw_image(&mut self, _: pdf::Image<'a, '_>, _: pdf::ImageDrawProps<'a>) {}
    fn push_clip_path(&mut self, path: &pdf::ClipPath) {
        self.push_clip_rect(&path.path.bounding_box());
    }
    fn push_clip_rect(&mut self, rect: &Rect) {
        self.clips
            .push(self.clips.last().unwrap_or(&self.crop).intersect(*rect));
    }
    fn push_transparency_group(&mut self, _: f32, _: Option<pdf::SoftMask<'a>>, _: pdf::BlendMode) {
    }
    fn pop_clip(&mut self) {
        self.clips.pop();
    }
    fn pop_transparency_group(&mut self) {}
}

struct Span {
    x: f64,
    text: String,
}
fn span_text(line: &[Glyph]) -> String {
    use unicode_bidi::{BidiClass, BidiInfo, bidi_class};
    let mut pieces = Vec::new();
    let mut previous: Option<&Glyph> = None;
    for g in line {
        if let Some(prev) = previous {
            // A fill-and-stroke text run is delivered once for each paint.
            if g.x == prev.x && g.y == prev.y && g.text == prev.text {
                continue;
            }
            // For complex scripts, individual glyph advances do not measure
            // the width of the shaped cluster. Prefer the PDF's word spaces.
            let shaped = prev.text.chars().chain(g.text.chars()).any(
                |c| matches!(c,'\u{900}'..='\u{dff}'|'\u{e00}'..='\u{eff}'|'\u{1780}'..='\u{17ff}'),
            );
            let word_gap = if shaped { 0.9 } else { 0.35 };
            if g.x - prev.end > g.size * word_gap && !prev.text.ends_with(' ') {
                pieces.push(" ");
            }
        }
        pieces.push(g.text.as_str());
        previous = Some(g);
    }
    // Positions are in visual order. Keep the logical Unicode value of each
    // glyph cluster (/ActualText can represent several codepoints) intact.
    if !pieces.iter().any(|s| {
        s.chars()
            .any(|c| matches!(bidi_class(c), BidiClass::R | BidiClass::AL))
    }) {
        return pieces.concat();
    }
    let labels: String = pieces
        .iter()
        .map(|s| s.chars().next().unwrap_or(' '))
        .collect();
    let info = BidiInfo::new(&labels, None);
    let levels: Vec<_> = labels.char_indices().map(|(i, _)| info.levels[i]).collect();
    BidiInfo::reorder_visual(&levels)
        .into_iter()
        .map(|i| pieces[i])
        .collect()
}
fn reading_order(mut glyphs: Vec<Glyph>) -> String {
    glyphs.sort_by(|a, b| b.y.total_cmp(&a.y));
    let mut rows = Vec::new();
    let mut rest = glyphs.as_mut_slice();
    while let Some(first) = rest.first() {
        let count = rest.partition_point(|g| first.y - g.y <= first.size * 0.28);
        let (line, remaining) = rest.split_at_mut(count.max(1));
        rest = remaining;
        line.sort_by(|a, b| a.x.total_cmp(&b.x));
        let spans: Vec<_> = line
            .chunk_by(|a, b| b.x - a.end < b.size * 2.1)
            .map(|part| Span {
                x: part[0].x,
                text: span_text(part),
            })
            .collect();
        rows.push(spans);
    }
    // Parallel runs with a stable gutter are columns. A full-width line
    // (heading or caption) closes the section; table rows keep their cells.
    let mut result = Vec::new();
    let mut i = 0;
    while i < rows.len() {
        let row = &rows[i];
        if row.len() == 2 {
            let n = rows[i..]
                .iter()
                .take_while(|r| {
                    r.len() == 2
                        && (r[0].x - row[0].x).abs() < 18.
                        && (r[1].x - row[1].x).abs() < 18.
                })
                .count();
            if n >= 2 {
                for col in 0..2 {
                    for r in &rows[i..i + n] {
                        result.push(r[col].text.trim().to_owned());
                    }
                }
                i += n;
                continue;
            }
        }
        result.push(
            row.iter()
                .map(|s| s.text.trim())
                .collect::<Vec<_>>()
                .join("\t"),
        );
        i += 1;
    }
    // Expand presentation ligatures without changing unrelated symbols or
    // combining-character sequences in the source.
    let mut text = String::new();
    for c in result.join("\n").chars() {
        match c {
            '\u{ad}' => {}
            '\u{fb00}'..='\u{fb06}' => text.extend(std::iter::once(c).nfkd()),
            _ if !c.is_control() || matches!(c, '\t' | '\n') => text.push(c),
            _ => {}
        }
    }
    text.trim().to_owned()
}
