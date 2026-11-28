use crate::metal::{Buffer, BufferSlice, Commands, Gpu, Pipeline};
use anyhow::{Result, ensure};
use bytemuck::{Pod, Zeroable};
use plamo3_translate_rs::{Tokenizer, TranslationRequest};
use safetensors::{Dtype, SafeTensors};
use std::{
    io::Read,
    path::Path,
    time::{Duration, Instant},
};

const D: usize = 1536;
const VOCAB: usize = 107520;
const BATCH: usize = 1024;
const DECODE_STEPS_PER_COMMIT: usize = 16;
const SCREEN_BLOCKS: usize = VOCAB / 64; // 1680 blocks x 64 rows
const SCREEN_CANDS: usize = SCREEN_BLOCKS * 4; // 6720 candidates -> exact Q6K_M rescore
const RESCORE_BLOCKS: usize = SCREEN_CANDS / 8; // 840 blocks x 8 candidates

unsafe extern "C" {
    fn CC_SHA256(data: *const u8, len: u32, md: *mut u8) -> *mut u8;
}

fn sha256_hex_hw(bytes: &[u8]) -> String {
    assert!(bytes.len() <= u32::MAX as usize);
    let mut digest = [0u8; 32];
    unsafe {
        CC_SHA256(bytes.as_ptr(), bytes.len() as u32, digest.as_mut_ptr());
    }
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(64);
    for b in digest {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct EmbedParams {
    ids_offset: u32,
    use_state: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct NormParams {
    token_offset: u32,
    write_res: u32,
    check_stop: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct PosParams {
    start_pos: u32,
    check_stop: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct MatvecParams {
    k: u32,
    n: u32,
    check_stop: u32,
    fused_swiglu: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct ScreenParams {
    check_stop: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct ArgmaxParams {
    num_blocks: u32,
    step_out_slot: u32,
    check_stop: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct MatmulParams {
    m: u32,
    n: u32,
    k: u32,
    fused_swiglu: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct AttnPrefillParams {
    start_pos: u32,
    count: u32,
    full: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct AttnDecodeParams {
    pos: u32,
    full: u32,
    check_stop: u32,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WeightKind {
    Q4K,
    Q6K,
}

#[derive(Clone, Copy)]
struct MatrixRef {
    off: usize,
    rows: usize,
    cols: usize,
    kind: WeightKind,
}

struct Kernels {
    embed_and_norm: Pipeline,
    fused_norm: Pipeline,
    prepare_qkv: Pipeline,
    matvec_q4k_nr2: Pipeline,
    matvec_q6k_nr2: Pipeline,
    matmul_q4k_64x64: Pipeline,
    matmul_q4k_32x64: Pipeline,
    matmul_q6k_64x64: Pipeline,
    matmul_q6k_32x64: Pipeline,
    lm_head_screen: Pipeline,
    lm_head_rescore: Pipeline,
    lm_head_full: Pipeline,
    argmax_final: Pipeline,
    attn_prefill: Pipeline,
    attn_decode: Pipeline,
}

struct Layer {
    qkv: MatrixRef,
    o: MatrixRef,
    up: MatrixRef,
    down: MatrixRef,
    attn_norm_off: usize,
    q_norm_off: usize,
    k_norm_off: usize,
    post_attn_off: usize,
    ffn_norm_off: usize,
    post_ffn_off: usize,
}

pub struct Engine {
    client: Gpu,
    weights: Buffer,
    kernels: Kernels,
    tokenizer: Tokenizer,
    embed_off: usize,
    head_screen_off: usize,
    head_q4_off: usize,
    final_norm_off: usize,
    layers: Vec<Layer>,
    rope: [Buffer; 2],
    valid_mask: Buffer,
    stop_mask: Buffer,
}

fn rope_table(full: bool) -> [f32; 65] {
    let base = if full { 1e6f32 } else { 1e4 };
    let bound = |beta: f32| 64. * (4096. / (beta * 2. * std::f32::consts::PI)).ln() / base.ln();
    let mut table = [0.; 65];
    for (i, t) in table[..64].iter_mut().enumerate() {
        let ramp = if full {
            ((i as f32 - bound(32.)) / (bound(1.) - bound(32.))).clamp(0., 1.)
        } else {
            0.
        };
        *t = base.powf(-(i as f32) / 64.) * (1. - ramp + ramp / 256.);
    }
    table[64] = if full { 1. + 0.1 * 256f32.ln() } else { 1. };
    table
}

fn build_vocab_masks(tokenizer: &Tokenizer) -> (Vec<u32>, Vec<u32>) {
    let words = VOCAB.div_ceil(32);
    let mut valid_mask = vec![0u32; words];
    let mut stop_mask = vec![0u32; words];
    for id in 0..VOCAB as u32 {
        let w = (id / 32) as usize;
        let b = 1u32 << (id & 31);
        if [1, 2, 16].contains(&id) {
            valid_mask[w] |= b;
            stop_mask[w] |= b;
            continue;
        }
        let Some(piece) = tokenizer.output_piece(id) else {
            continue;
        };
        if piece.is_empty()
            || piece
                .iter()
                .any(|&c| (c < 32 && !matches!(c, 9 | 10 | 13)) || c == 127)
        {
            continue;
        }
        match std::str::from_utf8(&piece) {
            Ok(_) => {
                valid_mask[w] |= b;
            }
            Err(e) if e.error_len().is_none() => {
                valid_mask[w] |= b;
                stop_mask[w] |= b;
            }
            Err(_) => {}
        }
    }
    (valid_mask, stop_mask)
}

impl Engine {
    pub fn open(path: &Path) -> Result<Self> {
        let mut f = std::fs::File::open(path)?;
        ensure!(
            f.metadata()?.len() == crate::MODEL_BYTES as u64,
            "翻訳モデルのサイズが一致しません。"
        );
        let client = Gpu::open()?;
        // Allocate ONE shared Metal buffer and read the file directly into it (zero extra copy).
        let weights = client.empty(crate::MODEL_BYTES)?;
        {
            let dst = unsafe { &mut weights.bytes_mut()[..crate::MODEL_BYTES] };
            let mut filled = 0;
            while filled < dst.len() {
                let n = f.read(&mut dst[filled..])?;
                ensure!(n > 0, "翻訳モデルの読み込みが途中で終了しました。");
                filled += n;
            }
            let mut extra = [0u8; 1];
            ensure!(
                f.read(&mut extra)? == 0,
                "翻訳モデルのサイズが一致しません。"
            );
            ensure!(
                sha256_hex_hw(dst) == crate::MODEL_SHA256,
                "翻訳モデルが同梱版と一致しません。"
            );
        }

        let kernels = Kernels {
            embed_and_norm: client.pipeline("embed_and_norm_q4k")?,
            fused_norm: client.pipeline("fused_post_add_pre_norm")?,
            prepare_qkv: client.pipeline("prepare_qkv")?,
            matvec_q4k_nr2: client.pipeline("matvec_q4k_u2_nr2")?,
            matvec_q6k_nr2: client.pipeline("matvec_q6k_u2_nr2")?,
            matmul_q4k_64x64: client.pipeline("matmul_q4k_64x64")?,
            matmul_q4k_32x64: client.pipeline("matmul_q4k_32x64")?,
            matmul_q6k_64x64: client.pipeline("matmul_q6k_64x64")?,
            matmul_q6k_32x64: client.pipeline("matmul_q6k_32x64")?,
            lm_head_screen: client.pipeline("lm_head_screen_1bit")?,
            lm_head_rescore: client.pipeline("lm_head_rescore_q6k")?,
            lm_head_full: client.pipeline("lm_head_full_q6k")?,
            argmax_final: client.pipeline("argmax_final")?,
            attn_prefill: client.pipeline("attention_prefill_mqa")?,
            attn_decode: client.pipeline("attention_decode_1pass")?,
        };

        let raw = unsafe { &weights.bytes()[..crate::MODEL_BYTES] };
        let base_ptr = raw.as_ptr() as usize;
        let file = SafeTensors::deserialize(raw)?;

        let offset_of = |name: &str, dtype: Dtype, shape: &[usize]| -> Result<usize> {
            let t = file.tensor(name)?;
            ensure!(
                t.dtype() == dtype && t.shape() == shape,
                "incorrect shape/type for {name}"
            );
            let off = t.data().as_ptr() as usize - base_ptr;
            ensure!(off % 16 == 0, "unaligned tensor {name} at {off}");
            Ok(off)
        };
        let matrix = |name: &str, rows: usize, cols: usize| -> Result<MatrixRef> {
            let q4_name = format!("{name}.q4k");
            if let Ok(off) = offset_of(&q4_name, Dtype::U8, &[rows, (cols / 512) * 288]) {
                return Ok(MatrixRef {
                    off,
                    rows,
                    cols,
                    kind: WeightKind::Q4K,
                });
            }
            let q6_name = format!("{name}.q6k");
            let off = offset_of(&q6_name, Dtype::U8, &[rows, (cols / 512) * 432])?;
            Ok(MatrixRef {
                off,
                rows,
                cols,
                kind: WeightKind::Q6K,
            })
        };

        let tokenizer = Tokenizer::from_jsonl(file.tensor("tokenizer.jsonl")?.data())
            .map_err(anyhow::Error::from_boxed)?;
        let (valid_bits, stop_bits) = build_vocab_masks(&tokenizer);
        let valid_mask = client.upload(bytemuck::cast_slice(&valid_bits))?;
        let stop_mask = client.upload(bytemuck::cast_slice(&stop_bits))?;

        let embed_off = offset_of(
            "token_embd.weight.q4k",
            Dtype::U8,
            &[VOCAB, (D / 512) * 288],
        )?;
        let head_screen_off =
            offset_of("output.weight.screen1", Dtype::U8, &[VOCAB, 240])?;
        let head_q4_off = offset_of(
            "output.weight.q6k",
            Dtype::U8,
            &[VOCAB, (D / 512) * 432],
        )?;
        let final_norm_off = offset_of("output_norm.weight", Dtype::F32, &[D])?;

        let mut layers = Vec::with_capacity(16);
        for i in 0..16 {
            let p = |s: &str| format!("blk.{i}.{s}");
            let norm = |s, n| offset_of(&p(s), Dtype::F32, &[n]);
            layers.push(Layer {
                qkv: matrix(&p("attn_qkv.weight"), 2304, D)?,
                o: matrix(&p("attn_output.weight"), D, 2048)?,
                up: matrix(&p("ffn_up.weight"), 12288, D)?,
                down: matrix(&p("ffn_down.weight"), D, 6144)?,
                attn_norm_off: norm("attn_norm.weight", D)?,
                q_norm_off: norm("attn_q_norm.weight", 128)?,
                k_norm_off: norm("attn_k_norm.weight", 128)?,
                post_attn_off: norm("post_attention_norm", D)?,
                ffn_norm_off: norm("ffn_norm.weight", D)?,
                post_ffn_off: norm("post_ffw_norm", D)?,
            });
        }
        let rope = [
            client.upload(bytemuck::cast_slice(&rope_table(false)))?,
            client.upload(bytemuck::cast_slice(&rope_table(true)))?,
        ];

        Ok(Self {
            client,
            weights,
            kernels,
            tokenizer,
            embed_off,
            head_screen_off,
            head_q4_off,
            final_norm_off,
            layers,
            rope,
            valid_mask,
            stop_mask,
        })
    }

    fn w(&self, off: usize) -> BufferSlice<'_> {
        self.weights.slice(off)
    }

    fn multiply(
        &self,
        cmd: &Commands,
        m: MatrixRef,
        x: BufferSlice<'_>,
        out: BufferSlice<'_>,
        count: usize,
        state: BufferSlice<'_>,
        check_stop: bool,
        fused_swiglu: bool,
    ) {
        if count >= 8 {
            let params = MatmulParams {
                m: count as u32,
                n: m.rows as u32,
                k: m.cols as u32,
                fused_swiglu: u32::from(fused_swiglu),
            };
            let (p64, p32) = match m.kind {
                WeightKind::Q4K => (&self.kernels.matmul_q4k_64x64, &self.kernels.matmul_q4k_32x64),
                WeightKind::Q6K => (&self.kernels.matmul_q6k_64x64, &self.kernels.matmul_q6k_32x64),
            };
            if count > 32 {
                cmd.dispatch(
                    p64,
                    (m.rows / 64, count.div_ceil(64)),
                    128,
                    &[self.w(m.off), x, out],
                    Some(&params),
                );
            } else {
                cmd.dispatch(
                    p32,
                    (m.rows / 64, count.div_ceil(32)),
                    128,
                    &[self.w(m.off), x, out],
                    Some(&params),
                );
            }
        } else {
            let params = MatvecParams {
                k: m.cols as u32,
                n: m.rows as u32,
                check_stop: u32::from(check_stop),
                fused_swiglu: u32::from(fused_swiglu),
            };
            let pipe = match m.kind {
                WeightKind::Q4K => &self.kernels.matvec_q4k_nr2,
                WeightKind::Q6K => &self.kernels.matvec_q6k_nr2,
            };
            cmd.dispatch(
                pipe,
                (m.rows / 4, count),
                64,
                &[self.w(m.off), x, out, state],
                Some(&params),
            );
        }
    }

    /// Evaluate the full exact Q6K_M output layer on a captured final-normalized FP32 activation.
    #[cfg(feature = "reference")]
    pub fn score_head(&self, hidden: &[f32]) -> Result<Vec<f32>> {
        ensure!(hidden.len() == D);
        let h16: Vec<half::f16> = hidden.iter().copied().map(half::f16::from_f32).collect();
        let x = self.client.upload(bytemuck::cast_slice(&h16))?;
        let scores = self.client.empty(VOCAB * 4)?;
        self.client.submit(|cmd| {
            cmd.dispatch::<u32>(
                &self.kernels.lm_head_full,
                (VOCAB / 4, 1),
                64,
                &[self.w(self.head_q4_off), x.as_slice(), scores.as_slice()],
                None,
            );
        })?;
        Ok(scores.read_scores())
    }

    pub fn translate(&self, source: &str, cancelled: impl Fn() -> bool) -> Result<Translation> {
        self.translate_with_mode(source, false, cancelled, |_| {}, false)
    }

    pub fn translate_stream(
        &self,
        source: &str,
        easy: bool,
        cancelled: impl Fn() -> bool,
        publish: impl FnMut(&str),
    ) -> Result<Translation> {
        self.translate_with_mode(source, easy, cancelled, publish, false)
    }

    /// Developer verification helper: run with full 107,520-row lm_head instead of 2-stage screen.
    pub fn translate_full_head(
        &self,
        source: &str,
        cancelled: impl Fn() -> bool,
    ) -> Result<Translation> {
        self.translate_with_mode(source, false, cancelled, |_| {}, true)
    }

    fn translate_with_mode(
        &self,
        source: &str,
        easy: bool,
        cancelled: impl Fn() -> bool,
        mut publish: impl FnMut(&str),
        force_full_head: bool,
    ) -> Result<Translation> {
        let start = Instant::now();
        ensure!(!cancelled(), "cancelled");
        let prompt = TranslationRequest {
            text: source,
            target: if easy { "ja-easy" } else { "ja" },
            form: (!easy).then_some("plain"),
            ..Default::default()
        }
        .prompt()
        .map_err(anyhow::Error::from_boxed)?;
        let tokens = self.tokenizer.encode(&prompt);
        ensure!(
            tokens.len() < 32768,
            "ページがモデルの入力上限を超えています。"
        );
        let limit = (2 * tokens.len() + 128).max(512).min(32768 - tokens.len());
        let mut session = Session::new(self, tokens.len() + limit, tokens.len().min(BATCH))?;

        let mut chunks = tokens.chunks(BATCH).peekable();
        while let Some(chunk) = chunks.next() {
            ensure!(!cancelled(), "cancelled");
            let is_last = chunks.peek().is_none();
            session.ids.write_slice(chunk);
            if is_last {
                session.state.write_u32_at(0, 0);
                session.state.write_u32_at(1, 0);
            }
            let pos = session.pos;
            self.client.submit(|cmd| {
                session.encode_prefill_chunk(cmd, chunk.len(), pos, is_last, force_full_head);
            })?;
            session.pos += chunk.len();
        }
        let prefill = start.elapsed();

        let mut next_id = if force_full_head {
            let scores = session.logits.read_scores();
            self.pick_valid_token(&scores, &[], true)?
        } else {
            session.state.read_u32_at(3)
        };
        let mut bytes = Vec::new();
        let mut valid = 0usize;
        let mut published = 0usize;
        let mut generated = 0usize;

        loop {
            ensure!(!cancelled(), "cancelled");
            if [1, 2, 16].contains(&next_id) {
                ensure!(valid == bytes.len(), "unexpected eos mid-utf8");
                if valid > published {
                    publish(std::str::from_utf8(&bytes[published..valid])?);
                }
                return Ok(Translation {
                    text: String::from_utf8(bytes)?,
                    input: tokens.len(),
                    output: generated,
                    prefill,
                    total: start.elapsed(),
                });
            }
            ensure!(generated < limit, "ページの翻訳が生成上限に達しました。");
            let piece = self
                .tokenizer
                .output_piece(next_id)
                .ok_or_else(|| anyhow::anyhow!("invalid token {next_id}"))?;
            bytes.extend_from_slice(&piece);
            valid +=
                std::str::from_utf8(&bytes[valid..]).map_or_else(|e| e.valid_up_to(), str::len);
            if valid > published {
                publish(std::str::from_utf8(&bytes[published..valid])?);
                published = valid;
            }
            generated += 1;

            if force_full_head || valid < bytes.len() {
                session.state.write_u32_at(0, 0);
                session.state.write_u32_at(1, 0);
                session.state.write_u32_at(2, next_id);
                let pos = session.pos;
                self.client.submit(|cmd| {
                    session.encode_decode_step(cmd, pos, 0, false, true);
                })?;
                session.pos += 1;
                let scores = session.logits.read_scores();
                next_id = self.pick_valid_token(&scores, &bytes[valid..], valid == bytes.len())?;
                continue;
            }

            let remaining = (limit + 1).saturating_sub(generated);
            let steps = remaining.min(DECODE_STEPS_PER_COMMIT);
            session.state.write_u32_at(0, 0);
            session.state.write_u32_at(1, 0);
            session.state.write_u32_at(2, next_id);
            let base_pos = session.pos;
            self.client.submit(|cmd| {
                for s in 0..steps {
                    session.encode_decode_step(cmd, base_pos + s, s, s > 0, false);
                }
            })?;

            let produced = session.state.read_u32_at(1) as usize;
            ensure!(
                produced >= 1 && produced <= steps,
                "bad batch count {produced}"
            );
            session.pos += produced;

            for s in 0..(produced - 1) {
                let id = session.state.read_u32_at(2 + s + 1);
                if [1, 2, 16].contains(&id) {
                    if valid > published {
                        publish(std::str::from_utf8(&bytes[published..valid])?);
                    }
                    return Ok(Translation {
                        text: String::from_utf8(bytes)?,
                        input: tokens.len(),
                        output: generated,
                        prefill,
                        total: start.elapsed(),
                    });
                }
                ensure!(generated < limit, "ページの翻訳が生成上限に達しました。");
                let piece = self
                    .tokenizer
                    .output_piece(id)
                    .ok_or_else(|| anyhow::anyhow!("invalid token {id}"))?;
                bytes.extend_from_slice(&piece);
                valid += std::str::from_utf8(&bytes[valid..])
                    .map_or_else(|e| e.valid_up_to(), str::len);
                generated += 1;
            }
            if valid > published {
                publish(std::str::from_utf8(&bytes[published..valid])?);
                published = valid;
            }
            next_id = session.state.read_u32_at(2 + produced);
        }
    }

    fn pick_valid_token(&self, scores: &[f32], pending: &[u8], allow_eos: bool) -> Result<u32> {
        let mut best = f32::NEG_INFINITY;
        let mut winner = None;
        for (id, &score) in scores.iter().enumerate() {
            if score <= best || !score.is_finite() {
                continue;
            }
            let id = id as u32;
            if [1, 2, 16].contains(&id) {
                if allow_eos {
                    best = score;
                    winner = Some(id);
                }
                continue;
            }
            let Some(piece) = self.tokenizer.output_piece(id) else {
                continue;
            };
            if piece.is_empty()
                || piece
                    .iter()
                    .any(|&b| (b < 32 && !matches!(b, 9 | 10 | 13)) || b == 127)
            {
                continue;
            }
            let mut tail = pending.to_vec();
            tail.extend_from_slice(&piece);
            if std::str::from_utf8(&tail).is_err_and(|e| e.error_len().is_some()) {
                continue;
            }
            best = score;
            winner = Some(id);
        }
        winner.ok_or_else(|| anyhow::anyhow!("no valid token"))
    }
}

pub struct Translation {
    pub text: String,
    pub input: usize,
    pub output: usize,
    pub prefill: Duration,
    pub total: Duration,
}

struct Session<'a> {
    engine: &'a Engine,
    pos: usize,
    cache: Vec<(Buffer, Buffer)>,
    ids: Buffer,
    state: Buffer,
    x: Buffer,
    tmp: Buffer,
    norm: Buffer,
    buf_a: Buffer,
    buf_b: Buffer,
    cand_ids: Buffer,
    partial_val: Buffer,
    partial_id: Buffer,
    logits: Buffer,
}

impl<'a> Session<'a> {
    fn new(engine: &'a Engine, capacity: usize, batch: usize) -> Result<Self> {
        let c = &engine.client;
        let cache = (0..16)
            .map(|_| {
                Ok((
                    c.empty_private(capacity * 128 * 2)?,
                    c.empty_private(capacity * 128 * 2)?,
                ))
            })
            .collect::<Result<Vec<_>>>()?;
        let b = batch.max(1);
        Ok(Self {
            engine,
            pos: 0,
            cache,
            ids: c.empty(b * 4)?,
            state: c.empty((4 + DECODE_STEPS_PER_COMMIT) * 4)?,
            x: c.empty_private(b * D * 4)?,
            tmp: c.empty_private(b * D * 4)?,
            norm: c.empty_private(b * D * 2)?,
            buf_a: c.empty_private(b * 2304 * 2)?,
            buf_b: c.empty_private(b * 6144 * 2)?,
            cand_ids: c.empty_private(SCREEN_CANDS * 4)?,
            partial_val: c.empty_private(RESCORE_BLOCKS * 4)?,
            partial_id: c.empty_private(RESCORE_BLOCKS * 4)?,
            logits: c.empty(VOCAB * 4)?,
        })
    }

    fn encode_head(
        &self,
        cmd: &Commands,
        step_out_slot: usize,
        check_stop: bool,
        full_logits: bool,
    ) {
        let e = self.engine;
        let k = &e.kernels;
        if full_logits {
            cmd.dispatch::<u32>(
                &k.lm_head_full,
                (VOCAB / 4, 1),
                64,
                &[
                    e.w(e.head_q4_off),
                    self.norm.as_slice(),
                    self.logits.as_slice(),
                ],
                None,
            );
            return;
        }
        let sp = ScreenParams {
            check_stop: u32::from(check_stop),
        };
        cmd.dispatch(
            &k.lm_head_screen,
            (SCREEN_BLOCKS, 1),
            256,
            &[
                e.w(e.head_screen_off),
                self.norm.as_slice(),
                e.valid_mask.as_slice(),
                self.cand_ids.as_slice(),
                self.state.as_slice(),
            ],
            Some(&sp),
        );
        cmd.dispatch(
            &k.lm_head_rescore,
            (RESCORE_BLOCKS, 1),
            128,
            &[
                e.w(e.head_q4_off),
                self.norm.as_slice(),
                self.cand_ids.as_slice(),
                self.partial_val.as_slice(),
                self.partial_id.as_slice(),
                self.state.as_slice(),
            ],
            Some(&sp),
        );
        let ap = ArgmaxParams {
            num_blocks: RESCORE_BLOCKS as u32,
            step_out_slot: step_out_slot as u32,
            check_stop: u32::from(check_stop),
        };
        cmd.dispatch(
            &k.argmax_final,
            (1, 1),
            256,
            &[
                self.partial_val.as_slice(),
                self.partial_id.as_slice(),
                e.stop_mask.as_slice(),
                self.state.as_slice(),
            ],
            Some(&ap),
        );
    }

    fn encode_prefill_chunk(
        &self,
        cmd: &Commands,
        n: usize,
        start_pos: usize,
        readout: bool,
        full_logits: bool,
    ) {
        let e = self.engine;
        let k = &e.kernels;

        let ep = EmbedParams {
            ids_offset: 0,
            use_state: 0,
        };
        cmd.dispatch(
            &k.embed_and_norm,
            (n, 1),
            192,
            &[
                e.w(e.embed_off),
                self.ids.as_slice(),
                e.w(e.layers[0].attn_norm_off),
                self.x.as_slice(),
                self.norm.as_slice(),
                self.state.as_slice(),
            ],
            Some(&ep),
        );

        for (i, layer) in e.layers.iter().enumerate() {
            let full = usize::from(i % 8 == 7);
            let (keys, values) = &self.cache[i];

            e.multiply(
                cmd,
                layer.qkv,
                self.norm.as_slice(),
                self.buf_a.as_slice(),
                n,
                self.state.as_slice(),
                false,
                false,
            );

            let pp = PosParams {
                start_pos: start_pos as u32,
                check_stop: 0,
            };
            cmd.dispatch(
                &k.prepare_qkv,
                (17, n),
                32,
                &[
                    self.buf_a.as_slice(),
                    e.w(layer.q_norm_off),
                    e.w(layer.k_norm_off),
                    e.rope[full].as_slice(),
                    self.buf_b.as_slice(),
                    keys.as_slice(),
                    values.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&pp),
            );

            if i == 15 {
                if !readout {
                    break;
                }
                // On the final layer during prefill readout, only token `n - 1` is needed for logits.
                let last_q_off = (n - 1) * 2048 * 2;
                let last_x_off = (n - 1) * D * 4;
                let ap = AttnDecodeParams {
                    pos: (start_pos + n - 1) as u32,
                    full: full as u32,
                    check_stop: 0,
                };
                cmd.dispatch(
                    &k.attn_decode,
                    (16, 1),
                    256,
                    &[
                        self.buf_b.slice(last_q_off),
                        keys.as_slice(),
                        values.as_slice(),
                        self.buf_a.as_slice(),
                        self.state.as_slice(),
                    ],
                    Some(&ap),
                );
                e.multiply(
                    cmd,
                    layer.o,
                    self.buf_a.as_slice(),
                    self.buf_b.as_slice(),
                    1,
                    self.state.as_slice(),
                    false,
                    false,
                );
                let np_attn = NormParams {
                    token_offset: 0,
                    write_res: 1,
                    check_stop: 0,
                };
                cmd.dispatch(
                    &k.fused_norm,
                    (1, 1),
                    192,
                    &[
                        self.buf_b.as_slice(),
                        self.x.slice(last_x_off),
                        e.w(layer.post_attn_off),
                        e.w(layer.ffn_norm_off),
                        self.tmp.as_slice(),
                        self.norm.as_slice(),
                        self.state.as_slice(),
                    ],
                    Some(&np_attn),
                );
                e.multiply(
                    cmd,
                    layer.up,
                    self.norm.as_slice(),
                    self.buf_b.as_slice(),
                    1,
                    self.state.as_slice(),
                    false,
                    true,
                );
                e.multiply(
                    cmd,
                    layer.down,
                    self.buf_b.as_slice(),
                    self.buf_a.as_slice(),
                    1,
                    self.state.as_slice(),
                    false,
                    false,
                );
                let np_final = NormParams {
                    token_offset: 0,
                    write_res: 0,
                    check_stop: 0,
                };
                cmd.dispatch(
                    &k.fused_norm,
                    (1, 1),
                    192,
                    &[
                        self.buf_a.as_slice(),
                        self.tmp.as_slice(),
                        e.w(layer.post_ffn_off),
                        e.w(e.final_norm_off),
                        self.x.as_slice(),
                        self.norm.as_slice(),
                        self.state.as_slice(),
                    ],
                    Some(&np_final),
                );
                break;
            }

            if n == 1 {
                let ap = AttnDecodeParams {
                    pos: start_pos as u32,
                    full: full as u32,
                    check_stop: 0,
                };
                cmd.dispatch(
                    &k.attn_decode,
                    (16, 1),
                    256,
                    &[
                        self.buf_b.as_slice(),
                        keys.as_slice(),
                        values.as_slice(),
                        self.buf_a.as_slice(),
                        self.state.as_slice(),
                    ],
                    Some(&ap),
                );
            } else {
                let ap = AttnPrefillParams {
                    start_pos: start_pos as u32,
                    count: n as u32,
                    full: full as u32,
                };
                cmd.dispatch(
                    &k.attn_prefill,
                    (n.div_ceil(2), 1),
                    128,
                    &[
                        self.buf_b.as_slice(),
                        keys.as_slice(),
                        values.as_slice(),
                        self.buf_a.as_slice(),
                    ],
                    Some(&ap),
                );
            }

            e.multiply(
                cmd,
                layer.o,
                self.buf_a.as_slice(),
                self.buf_b.as_slice(),
                n,
                self.state.as_slice(),
                false,
                false,
            );

            let np_attn = NormParams {
                token_offset: 0,
                write_res: 1,
                check_stop: 0,
            };
            cmd.dispatch(
                &k.fused_norm,
                (n, 1),
                192,
                &[
                    self.buf_b.as_slice(),
                    self.x.as_slice(),
                    e.w(layer.post_attn_off),
                    e.w(layer.ffn_norm_off),
                    self.tmp.as_slice(),
                    self.norm.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&np_attn),
            );

            e.multiply(
                cmd,
                layer.up,
                self.norm.as_slice(),
                self.buf_b.as_slice(),
                n,
                self.state.as_slice(),
                false,
                true,
            );

            e.multiply(
                cmd,
                layer.down,
                self.buf_b.as_slice(),
                self.buf_a.as_slice(),
                n,
                self.state.as_slice(),
                false,
                false,
            );

            let np_ffn = NormParams {
                token_offset: 0,
                write_res: 1,
                check_stop: 0,
            };
            cmd.dispatch(
                &k.fused_norm,
                (n, 1),
                192,
                &[
                    self.buf_a.as_slice(),
                    self.tmp.as_slice(),
                    e.w(layer.post_ffn_off),
                    e.w(e.layers[i + 1].attn_norm_off),
                    self.x.as_slice(),
                    self.norm.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&np_ffn),
            );
        }

        if readout {
            self.encode_head(cmd, 1, false, full_logits);
        }
    }

    fn encode_decode_step(
        &self,
        cmd: &Commands,
        pos: usize,
        step_idx: usize,
        check_stop: bool,
        full_logits: bool,
    ) {
        let e = self.engine;
        let k = &e.kernels;
        let cs = u32::from(check_stop);

        let ep = EmbedParams {
            ids_offset: step_idx as u32,
            use_state: 1,
        };
        cmd.dispatch(
            &k.embed_and_norm,
            (1, 1),
            192,
            &[
                e.w(e.embed_off),
                self.ids.as_slice(),
                e.w(e.layers[0].attn_norm_off),
                self.x.as_slice(),
                self.norm.as_slice(),
                self.state.as_slice(),
            ],
            Some(&ep),
        );

        for (i, layer) in e.layers.iter().enumerate() {
            let full = usize::from(i % 8 == 7);
            let (keys, values) = &self.cache[i];

            e.multiply(
                cmd,
                layer.qkv,
                self.norm.as_slice(),
                self.buf_a.as_slice(),
                1,
                self.state.as_slice(),
                check_stop,
                false,
            );

            let pp = PosParams {
                start_pos: pos as u32,
                check_stop: cs,
            };
            cmd.dispatch(
                &k.prepare_qkv,
                (17, 1),
                32,
                &[
                    self.buf_a.as_slice(),
                    e.w(layer.q_norm_off),
                    e.w(layer.k_norm_off),
                    e.rope[full].as_slice(),
                    self.buf_b.as_slice(),
                    keys.as_slice(),
                    values.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&pp),
            );

            let ap = AttnDecodeParams {
                pos: pos as u32,
                full: full as u32,
                check_stop: cs,
            };
            cmd.dispatch(
                &k.attn_decode,
                (16, 1),
                256,
                &[
                    self.buf_b.as_slice(),
                    keys.as_slice(),
                    values.as_slice(),
                    self.buf_a.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&ap),
            );

            e.multiply(
                cmd,
                layer.o,
                self.buf_a.as_slice(),
                self.buf_b.as_slice(),
                1,
                self.state.as_slice(),
                check_stop,
                false,
            );

            let np_attn = NormParams {
                token_offset: 0,
                write_res: 1,
                check_stop: cs,
            };
            cmd.dispatch(
                &k.fused_norm,
                (1, 1),
                192,
                &[
                    self.buf_b.as_slice(),
                    self.x.as_slice(),
                    e.w(layer.post_attn_off),
                    e.w(layer.ffn_norm_off),
                    self.tmp.as_slice(),
                    self.norm.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&np_attn),
            );

            e.multiply(
                cmd,
                layer.up,
                self.norm.as_slice(),
                self.buf_b.as_slice(),
                1,
                self.state.as_slice(),
                check_stop,
                true,
            );

            e.multiply(
                cmd,
                layer.down,
                self.buf_b.as_slice(),
                self.buf_a.as_slice(),
                1,
                self.state.as_slice(),
                check_stop,
                false,
            );

            let next_norm_off = if i + 1 < 16 {
                e.layers[i + 1].attn_norm_off
            } else {
                e.final_norm_off
            };
            let np_ffn = NormParams {
                token_offset: 0,
                write_res: u32::from(i + 1 < 16),
                check_stop: cs,
            };
            cmd.dispatch(
                &k.fused_norm,
                (1, 1),
                192,
                &[
                    self.buf_a.as_slice(),
                    self.tmp.as_slice(),
                    e.w(layer.post_ffn_off),
                    e.w(next_norm_off),
                    self.x.as_slice(),
                    self.norm.as_slice(),
                    self.state.as_slice(),
                ],
                Some(&np_ffn),
            );
        }

        self.encode_head(cmd, step_idx + 1, check_stop, full_logits);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use half::f16;

    #[test]
    fn q4k_and_q6k_decode_prefill_and_fused_swiglu_match_scalar_reference() -> Result<()> {
        let gpu = Gpu::open()?;
        let mv_q4_nr2 = gpu.pipeline("matvec_q4k_u2_nr2")?;
        let mm_q4_32 = gpu.pipeline("matmul_q4k_32x64")?;
        let mm_q4_64 = gpu.pipeline("matmul_q4k_64x64")?;
        let mv_q6_nr2 = gpu.pipeline("matvec_q6k_u2_nr2")?;
        let mm_q6_32 = gpu.pipeline("matmul_q6k_32x64")?;
        let mm_q6_64 = gpu.pipeline("matmul_q6k_64x64")?;
        let state = gpu.upload(bytemuck::cast_slice(&[0u32; 4]))?;

        let (rows, cols) = (128usize, 1536usize);
        let sbs = cols / 512;

        // Build synthetic Q4K_M matrix + scalar reference
        let mut q4_data = vec![0u8; rows * sbs * 288];
        let mut q4_ref = vec![0.0f32; rows * cols];
        for r in 0..rows {
            for sb in 0..sbs {
                let off = (r * sbs + sb) * 288;
                for blk in 0..2 {
                    let hdr = &mut q4_data[off + blk * 16..off + (blk + 1) * 16];
                    let d = f16::from_f32((((r * 5 + sb * 7 + blk * 3) % 19) as f32 + 1.) / 1200.);
                    let dmin =
                        f16::from_f32((((r * 3 + sb * 11 + blk * 5) % 17) as f32 + 1.) / 1500.);
                    hdr[0..2].copy_from_slice(&d.to_le_bytes());
                    hdr[2..4].copy_from_slice(&dmin.to_le_bytes());
                    let mut sc6 = [0u8; 8];
                    let mut m6 = [0u8; 8];
                    for g in 0..8 {
                        sc6[g] = ((r * 7 + sb * 13 + blk * 5 + g * 11) % 63 + 1) as u8;
                        m6[g] = ((r * 11 + sb * 3 + blk * 7 + g * 13) % 63) as u8;
                    }
                    for g in 0..4 {
                        hdr[4 + g] = (sc6[g] & 63) | ((sc6[g + 4] >> 4) << 6);
                        hdr[8 + g] = (m6[g] & 63) | ((m6[g + 4] >> 4) << 6);
                        hdr[12 + g] = (sc6[g + 4] & 15) | ((m6[g + 4] & 15) << 4);
                    }
                    for g in 0..8 {
                        let sc_f = d.to_f32() * sc6[g] as f32;
                        let mn_f = -dmin.to_f32() * m6[g] as f32;
                        for w in 0..4 {
                            let bits = ((r * 131 + sb * 17 + blk * 29 + g * 19 + w * 23) as u32)
                                .wrapping_mul(0x9e3779b9)
                                .rotate_right(5);
                            let qoff = off + 32 + (blk * 32 + g * 4 + w) * 4;
                            q4_data[qoff..qoff + 4].copy_from_slice(&bits.to_le_bytes());
                            for j in 0..8 {
                                let code = ((bits >> (4 * j)) & 15) as f32;
                                q4_ref[r * cols + sb * 512 + blk * 256 + g * 32 + w * 8 + j] =
                                    code * sc_f + mn_f;
                            }
                        }
                    }
                }
            }
        }
        let q4_buf = gpu.upload(&q4_data)?;

        // Build synthetic Q6K_M matrix + scalar reference
        let mut q6_data = vec![0u8; rows * sbs * 432];
        let mut q6_ref = vec![0.0f32; rows * cols];
        for r in 0..rows {
            for sb in 0..sbs {
                let off = (r * sbs + sb) * 432;
                let d0 = f16::from_f32((((r * 3 + sb * 5) % 17) as f32 + 1.) / 2000.);
                let d1 = f16::from_f32((((r * 7 + sb * 11) % 19) as f32 + 1.) / 2000.);
                q6_data[off..off + 2].copy_from_slice(&d0.to_le_bytes());
                q6_data[off + 2..off + 4].copy_from_slice(&d1.to_le_bytes());
                for g in 0..32 {
                    let s = (((r * 13 + sb * 7 + g * 17) % 111) as i32 - 55) as i8;
                    q6_data[off + 16 + g] = s as u8;
                    let df = if g < 16 { d0.to_f32() } else { d1.to_f32() };
                    let sc = df * s as f32;
                    let ql_lo = ((r * 131 + sb * 17 + g * 19) as u32)
                        .wrapping_mul(0x9e3779b9)
                        .rotate_right(3);
                    let ql_hi = ((r * 97 + sb * 23 + g * 31) as u32)
                        .wrapping_mul(0x85ebca6b)
                        .rotate_right(7);
                    let qh = ((r * 53 + sb * 41 + g * 67) as u32)
                        .wrapping_mul(0xc2b2ae35)
                        .rotate_right(11);
                    q6_data[off + 48 + g * 8..off + 48 + g * 8 + 4]
                        .copy_from_slice(&ql_lo.to_le_bytes());
                    q6_data[off + 48 + g * 8 + 4..off + 48 + g * 8 + 8]
                        .copy_from_slice(&ql_hi.to_le_bytes());
                    q6_data[off + 304 + g * 4..off + 304 + g * 4 + 4]
                        .copy_from_slice(&qh.to_le_bytes());
                    for j in 0..16 {
                        let lo = if j < 8 {
                            (ql_lo >> (4 * j)) & 15
                        } else {
                            (ql_hi >> (4 * (j - 8))) & 15
                        };
                        let hi = (qh >> (2 * j)) & 3;
                        let q = ((lo | (hi << 4)) as i32 - 32) as f32;
                        q6_ref[r * cols + sb * 512 + g * 16 + j] = sc * q;
                    }
                }
            }
        }
        let q6_buf = gpu.upload(&q6_data)?;

        for batch in [1usize, 7, 8, 31, 33, 65] {
            let source: Vec<f16> = (0..cols * batch)
                .map(|i| f16::from_f32(((i * 59 % 103) as f32 - 51.) / 71.))
                .collect();
            let source_f32: Vec<f32> = source.iter().map(|v| v.to_f32()).collect();
            let x = gpu.upload(bytemuck::cast_slice(&source))?;
            let y = gpu.empty(rows * batch * 2)?;

            for fused in [false, true] {
                if batch < 8 {
                    let vp = MatvecParams {
                        k: cols as u32,
                        n: rows as u32,
                        check_stop: 0,
                        fused_swiglu: u32::from(fused),
                    };
                    gpu.submit(|cmd| {
                        cmd.dispatch(
                            &mv_q4_nr2,
                            (rows / 4, batch),
                            64,
                            &[q4_buf.as_slice(), x.as_slice(), y.as_slice(), state.as_slice()],
                            Some(&vp),
                        );
                    })?;
                    verify_output("q4_nr2", &y, &q4_ref, &source_f32, rows, cols, batch, fused);
                } else {
                    let mp = MatmulParams {
                        m: batch as u32,
                        n: rows as u32,
                        k: cols as u32,
                        fused_swiglu: u32::from(fused),
                    };
                    gpu.submit(|cmd| {
                        if batch > 32 {
                            cmd.dispatch(
                                &mm_q4_64,
                                (rows / 64, batch.div_ceil(64)),
                                128,
                                &[q4_buf.as_slice(), x.as_slice(), y.as_slice()],
                                Some(&mp),
                            );
                        } else {
                            cmd.dispatch(
                                &mm_q4_32,
                                (rows / 64, batch.div_ceil(32)),
                                128,
                                &[q4_buf.as_slice(), x.as_slice(), y.as_slice()],
                                Some(&mp),
                            );
                        }
                    })?;
                    verify_output("q4_mm", &y, &q4_ref, &source_f32, rows, cols, batch, fused);
                }
            }

            // Test Q6K_M decode and prefill
            if batch < 8 {
                let vp = MatvecParams {
                    k: cols as u32,
                    n: rows as u32,
                    check_stop: 0,
                    fused_swiglu: 0,
                };
                gpu.submit(|cmd| {
                    cmd.dispatch(
                        &mv_q6_nr2,
                        (rows / 4, batch),
                        64,
                        &[q6_buf.as_slice(), x.as_slice(), y.as_slice(), state.as_slice()],
                        Some(&vp),
                    );
                })?;
                verify_output("q6_nr2", &y, &q6_ref, &source_f32, rows, cols, batch, false);
            } else {
                let mp = MatmulParams {
                    m: batch as u32,
                    n: rows as u32,
                    k: cols as u32,
                    fused_swiglu: 0,
                };
                gpu.submit(|cmd| {
                    if batch > 32 {
                        cmd.dispatch(
                            &mm_q6_64,
                            (rows / 64, batch.div_ceil(64)),
                            128,
                            &[q6_buf.as_slice(), x.as_slice(), y.as_slice()],
                            Some(&mp),
                        );
                    } else {
                        cmd.dispatch(
                            &mm_q6_32,
                            (rows / 64, batch.div_ceil(32)),
                            128,
                            &[q6_buf.as_slice(), x.as_slice(), y.as_slice()],
                            Some(&mp),
                        );
                    }
                })?;
                verify_output("q6_mm", &y, &q6_ref, &source_f32, rows, cols, batch, false);
            }
        }
        Ok(())
    }

    fn verify_output(
        label: &str,
        y: &Buffer,
        wref: &[f32],
        source: &[f32],
        rows: usize,
        cols: usize,
        batch: usize,
        fused: bool,
    ) {
        let out_cols = if fused { rows / 2 } else { rows };
        let result: Vec<f16> = y.read_slice(out_cols * batch);
        for token in [0, batch - 1] {
            for c in [0, 3, 17, out_cols - 1] {
                let expected = if fused {
                    let mut g = 0.0f32;
                    let mut u = 0.0f32;
                    for k in 0..cols {
                        let xv = source[token * cols + k];
                        g += wref[(2 * c) * cols + k] * xv;
                        u += wref[(2 * c + 1) * cols + k] * xv;
                    }
                    (g / (1.0 + (-g).exp())) * u
                } else {
                    let mut s = 0.0f32;
                    for k in 0..cols {
                        s += wref[c * cols + k] * source[token * cols + k];
                    }
                    s
                };
                let got = result[token * out_cols + c].to_f32();
                let tol = (expected.abs() * 0.01).max(0.03);
                assert!(
                    (got - expected).abs() < tol,
                    "{label} fused={fused} batch={batch} tok={token} c={c} want={expected} got={got}"
                );
            }
        }
    }

    #[test]
    fn attention_respects_causal_and_sliding_boundaries() -> Result<()> {
        let gpu = Gpu::open()?;
        let prefill_pipe = gpu.pipeline("attention_prefill_mqa")?;
        let dec_pipe = gpu.pipeline("attention_decode_1pass")?;
        let n = 3usize;
        let start = 2047usize;
        let q: Vec<f16> = (0..n * 2048)
            .map(|i| f16::from_f32(((i * 13 % 97) as f32 - 48.) / 50.))
            .collect();
        let k: Vec<f16> = (0..(start + n) * 128)
            .map(|i| f16::from_f32(((i * 17 % 127) as f32 - 63.) / 48.))
            .collect();
        let v: Vec<f16> = (0..k.len())
            .map(|i| {
                f16::from_f32(if i < 256 {
                    90.
                } else {
                    ((i * 31 % 151) as f32 - 75.) / 64.
                })
            })
            .collect();
        let qq = gpu.upload(bytemuck::cast_slice(&q))?;
        let kk = gpu.upload(bytemuck::cast_slice(&k))?;
        let vv = gpu.upload(bytemuck::cast_slice(&v))?;
        let out = gpu.empty(n * 2048 * 2)?;
        let state = gpu.upload(bytemuck::cast_slice(&[0u32; 4]))?;

        for full in [false, true] {
            let ap = AttnPrefillParams {
                start_pos: start as u32,
                count: n as u32,
                full: u32::from(full),
            };
            gpu.submit(|cmd| {
                cmd.dispatch(
                    &prefill_pipe,
                    (n.div_ceil(2), 1),
                    128,
                    &[qq.as_slice(), kk.as_slice(), vv.as_slice(), out.as_slice()],
                    Some(&ap),
                );
            })?;
            let result: Vec<f16> = out.read_slice(n * 2048);
            for t in 0..n {
                let end = start + t + 1;
                let first = if full { 0 } else { end.saturating_sub(2048) };
                for h in [0, 7, 15] {
                    let query = &q[(16 * t + h) * 128..][..128];
                    let scores: Vec<f64> = (first..end)
                        .map(|p| {
                            let dot: f64 = query
                                .iter()
                                .zip(&k[p * 128..])
                                .map(|(a, b)| f64::from(a.to_f32()) * f64::from(b.to_f32()))
                                .sum();
                            (dot / 128f64.sqrt()).exp()
                        })
                        .collect();
                    let sum: f64 = scores.iter().sum();
                    for col in [0, 31, 79, 127] {
                        let want = scores
                            .iter()
                            .enumerate()
                            .map(|(j, s)| s * f64::from(v[(first + j) * 128 + col].to_f32()))
                            .sum::<f64>()
                            / sum;
                        let got = f64::from(result[(16 * t + h) * 128 + col].to_f32());
                        let tol = (want.abs() * 0.005).max(0.01);
                        assert!(
                            (got - want).abs() < tol,
                            "prefill t={t} full={full} h={h} col={col} want={want} got={got}"
                        );
                    }
                }
            }

            let q2 = gpu.upload(bytemuck::cast_slice(&q[2 * 2048..3 * 2048]))?;
            let out1 = gpu.empty(2048 * 2)?;
            let dp = AttnDecodeParams {
                pos: (start + 2) as u32,
                full: u32::from(full),
                check_stop: 0,
            };
            gpu.submit(|cmd| {
                cmd.dispatch(
                    &dec_pipe,
                    (16, 1),
                    256,
                    &[
                        q2.as_slice(),
                        kk.as_slice(),
                        vv.as_slice(),
                        out1.as_slice(),
                        state.as_slice(),
                    ],
                    Some(&dp),
                );
            })?;
            let dec_res: Vec<f16> = out1.read_slice(2048);
            let end = start + 3;
            let first = if full { 0 } else { end.saturating_sub(2048) };
            for h in [0, 7, 15] {
                let query = &q[(16 * 2 + h) * 128..][..128];
                let scores: Vec<f64> = (first..end)
                    .map(|p| {
                        let dot: f64 = query
                            .iter()
                            .zip(&k[p * 128..])
                            .map(|(a, b)| f64::from(a.to_f32()) * f64::from(b.to_f32()))
                            .sum();
                        (dot / 128f64.sqrt()).exp()
                    })
                    .collect();
                let sum: f64 = scores.iter().sum();
                for col in [0, 31, 79, 127] {
                    let want = scores
                        .iter()
                        .enumerate()
                        .map(|(j, s)| s * f64::from(v[(first + j) * 128 + col].to_f32()))
                        .sum::<f64>()
                        / sum;
                    let got = f64::from(dec_res[h * 128 + col].to_f32());
                    let tol = (want.abs() * 0.005).max(0.01);
                    assert!(
                        (got - want).abs() < tol,
                        "decode full={full} h={h} col={col} want={want} got={got}"
                    );
                }
            }
        }
        Ok(())
    }
}
