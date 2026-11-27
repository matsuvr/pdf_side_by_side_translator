use crate::metal::{Buffer, Commands, Gpu, Pipeline};
use anyhow::{Result, ensure};
use plamo3_translate_rs::{Tokenizer, TranslationRequest};
use safetensors::{Dtype, SafeTensors};
use std::{
    io::Read,
    path::Path,
    time::{Duration, Instant},
};

const D: usize = 1536;
const VOCAB: usize = 107520;
const BATCH: usize = 128;

struct Matrix {
    q: Buffer,
    scale: Buffer,
    rows: usize,
    mv: Pipeline,
    mm: Option<Pipeline>,
}
impl Matrix {
    fn multiply(&self, commands: &Commands, x: &Buffer, out: &Buffer, count: usize) {
        let args = [&self.q, &self.scale, x, out];
        if count >= 8 {
            commands.dispatch(
                self.mm.as_ref().expect("body matrix"),
                (self.rows / 64, count.div_ceil(32)),
                256,
                &args,
                Some(count as u32),
            );
        } else {
            commands.dispatch(&self.mv, (self.rows / 4, count), 128, &args, None);
        }
    }
}

struct Kernels {
    embed: Pipeline,
    swiglu: Pipeline,
    norm: [Pipeline; 2],
    prepare: Pipeline,
    attention: [Pipeline; 2],
}
struct Layer {
    qkv: Matrix,
    o: Matrix,
    up: Matrix,
    down: Matrix,
    attn_norm: Buffer,
    q_norm: Buffer,
    k_norm: Buffer,
    post_attn: Buffer,
    ffn_norm: Buffer,
    post_ffn: Buffer,
}
pub struct Engine {
    client: Gpu,
    kernels: Kernels,
    tokenizer: Tokenizer,
    embed: Matrix,
    head: Matrix,
    final_norm: Buffer,
    layers: Vec<Layer>,
    rope: [Buffer; 2],
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

impl Engine {
    pub fn open(path: &Path) -> Result<Self> {
        use sha2::{Digest, Sha256};
        let f = std::fs::File::open(path)?;
        ensure!(
            f.metadata()?.len() == crate::MODEL_BYTES as u64,
            "翻訳モデルのサイズが一致しません。"
        );
        let mut data = Vec::new();
        data.try_reserve_exact(crate::MODEL_BYTES + 1)?;
        f.take(crate::MODEL_BYTES as u64 + 1)
            .read_to_end(&mut data)?;
        ensure!(
            data.len() == crate::MODEL_BYTES
                && format!("{:x}", Sha256::digest(&data)) == crate::MODEL_SHA256,
            "翻訳モデルが同梱版と一致しません。"
        );
        let file = SafeTensors::deserialize(&data)?;
        let client = Gpu::open()?;
        let kernels = Kernels {
            embed: client.pipeline("embed")?,
            swiglu: client.pipeline("swiglu")?,
            norm: [client.pipeline("norm_0")?, client.pipeline("norm_1")?],
            prepare: client.pipeline("prepare_qkv")?,
            attention: [
                client.pipeline("attention_0")?,
                client.pipeline("attention_1")?,
            ],
        };
        let load = |name: &str, dtype: Dtype, shape: &[usize]| -> Result<Buffer> {
            let t = file.tensor(name)?;
            ensure!(
                t.dtype() == dtype && t.shape() == shape,
                "incorrect shape/type for {name}"
            );
            client.upload(t.data())
        };
        let matrix = |name: &str, rows, cols| -> Result<Matrix> {
            Ok(Matrix {
                q: load(&format!("{name}.q4"), Dtype::U32, &[rows, cols / 8])?,
                scale: load(&format!("{name}.scale"), Dtype::F16, &[rows, cols / 32])?,
                rows,
                mv: client.pipeline(&format!("mv_{cols}_{rows}"))?,
                mm: if rows == VOCAB {
                    None
                } else {
                    Some(client.pipeline(&format!("mm_{cols}_{rows}"))?)
                },
            })
        };
        let tokenizer = Tokenizer::from_jsonl(file.tensor("tokenizer.jsonl")?.data())
            .map_err(anyhow::Error::from_boxed)?;
        let embed = matrix("model.embed_tokens.weight", VOCAB, D)?;
        let head = matrix("lm_head.weight", VOCAB, D)?;
        let final_norm = load("model.norm.weight", Dtype::F32, &[D])?;
        let mut layers = Vec::new();
        for i in 0..16 {
            let name = |s: &str| format!("model.layers.layers.{i}.{s}.weight");
            let norm = |s, n| load(&name(s), Dtype::F32, &[n]);
            layers.push(Layer {
                qkv: matrix(&name("mixer.qkv_proj"), 2304, D)?,
                o: matrix(&name("mixer.o_proj"), D, 2048)?,
                up: matrix(&name("mlp.gate_up_proj"), 12288, D)?,
                down: matrix(&name("mlp.down_proj"), D, 6144)?,
                attn_norm: norm("pre_mixer_norm", D)?,
                q_norm: norm("mixer.q_norm", 128)?,
                k_norm: norm("mixer.k_norm", 128)?,
                post_attn: norm("post_mixer_norm", D)?,
                ffn_norm: norm("pre_mlp_norm", D)?,
                post_ffn: norm("post_mlp_norm", D)?,
            });
        }
        let rope = [
            client.upload(bytemuck::cast_slice(&rope_table(false)))?,
            client.upload(bytemuck::cast_slice(&rope_table(true)))?,
        ];
        Ok(Self {
            client,
            kernels,
            tokenizer,
            embed,
            head,
            final_norm,
            layers,
            rope,
        })
    }

    /// Evaluate the quantized output layer on a captured final-normalized activation.
    #[cfg(feature = "reference")]
    pub fn score_head(&self, hidden: &[f32]) -> Result<Vec<f32>> {
        ensure!(hidden.len() == D);
        let x = self.client.upload(bytemuck::cast_slice(hidden))?;
        let scores = self.client.empty(VOCAB * 4)?;
        self.client
            .submit(|cmd| self.head.multiply(cmd, &x, &scores, 1))?;
        Ok(scores.read_scores())
    }

    pub fn translate(&self, source: &str, cancelled: impl Fn() -> bool) -> Result<Translation> {
        let start = Instant::now();
        ensure!(!cancelled(), "cancelled");
        let prompt = TranslationRequest {
            text: source,
            target: "ja",
            form: Some("plain"),
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
            session.forward(chunk, chunks.peek().is_none())?;
        }
        let mut scores = session.logits.read_scores();
        let prefill = start.elapsed();
        let mut bytes = Vec::new();
        let mut valid = 0;
        for generated in 0..=limit {
            ensure!(!cancelled(), "cancelled");
            let mut best = f32::NEG_INFINITY;
            let mut winner = None;
            for (id, &score) in scores.iter().enumerate() {
                if score <= best || !score.is_finite() {
                    continue;
                }
                let id = id as u32;
                if [1, 2, 16].contains(&id) {
                    if valid == bytes.len() {
                        best = score;
                        winner = Some((id, Vec::new()));
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
                let mut tail = bytes[valid..].to_vec();
                tail.extend_from_slice(&piece);
                if std::str::from_utf8(&tail).is_err_and(|e| e.error_len().is_some()) {
                    continue;
                }
                best = score;
                winner = Some((id, piece));
            }
            let (id, piece) = winner.ok_or_else(|| anyhow::anyhow!("no valid token"))?;
            if piece.is_empty() {
                return Ok(Translation {
                    text: String::from_utf8(bytes)?,
                    input: tokens.len(),
                    output: generated,
                    prefill,
                    total: start.elapsed(),
                });
            }
            ensure!(generated < limit, "ページの翻訳が生成上限に達しました。");
            bytes.extend_from_slice(&piece);
            valid +=
                std::str::from_utf8(&bytes[valid..]).map_or_else(|e| e.valid_up_to(), str::len);
            session.forward(&[id], true)?;
            scores = session.logits.read_scores();
        }
        unreachable!()
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
    x: Buffer,
    tmp: Buffer,
    norm: Buffer,
    qkv: Buffer,
    q: Buffer,
    attn: Buffer,
    ff: Buffer,
    gated: Buffer,
    proj: Buffer,
    logits: Buffer,
}
impl<'a> Session<'a> {
    fn new(engine: &'a Engine, capacity: usize, batch: usize) -> Result<Self> {
        let c = &engine.client;
        let cache = (0..16)
            .map(|_| Ok((c.empty(capacity * 128 * 2)?, c.empty(capacity * 128 * 2)?)))
            .collect::<Result<Vec<_>>>()?;
        let scratch = |width| c.empty(batch.next_multiple_of(32) * width * 4);
        Ok(Self {
            engine,
            pos: 0,
            cache,
            x: scratch(D)?,
            tmp: scratch(D)?,
            norm: scratch(D)?,
            qkv: scratch(2304)?,
            q: scratch(2048)?,
            attn: scratch(2048)?,
            ff: scratch(12288)?,
            gated: scratch(6144)?,
            proj: scratch(D)?,
            logits: c.empty(VOCAB * 4)?,
        })
    }
    fn forward(&mut self, ids: &[u32], readout: bool) -> Result<()> {
        let e = self.engine;
        let n = ids.len();
        let ids_buf = e.client.upload(bytemuck::cast_slice(ids))?;
        let k = &e.kernels;
        e.client.submit(|cmd| {
            cmd.dispatch(
                &k.embed,
                (n * D / 128, 1),
                128,
                &[&e.embed.q, &e.embed.scale, &ids_buf, &self.x],
                None,
            );
            let normalize = |src: &Buffer, w: &Buffer, res: &Buffer, dst: &Buffer, add: bool| {
                cmd.dispatch(
                    &k.norm[usize::from(add)],
                    (n, 1),
                    32,
                    &[src, w, res, dst],
                    Some(0),
                );
            };
            for (i, layer) in e.layers.iter().enumerate() {
                let full = usize::from(i % 8 == 7);
                let (keys, values) = &self.cache[i];
                normalize(&self.x, &layer.attn_norm, &self.x, &self.norm, false);
                layer.qkv.multiply(cmd, &self.norm, &self.qkv, n);
                cmd.dispatch(
                    &k.prepare,
                    (17, n),
                    32,
                    &[
                        &self.qkv,
                        &layer.q_norm,
                        &layer.k_norm,
                        &e.rope[full],
                        &self.q,
                        keys,
                        values,
                    ],
                    Some(self.pos as u32),
                );
                cmd.dispatch(
                    &k.attention[full],
                    (16, n),
                    256,
                    &[&self.q, keys, values, &self.attn],
                    Some(self.pos as u32),
                );
                layer.o.multiply(cmd, &self.attn, &self.proj, n);
                normalize(&self.proj, &layer.post_attn, &self.x, &self.tmp, true);
                normalize(&self.tmp, &layer.ffn_norm, &self.tmp, &self.norm, false);
                layer.up.multiply(cmd, &self.norm, &self.ff, n);
                cmd.dispatch(
                    &k.swiglu,
                    (n * 6144 / 128, 1),
                    128,
                    &[&self.ff, &self.gated],
                    None,
                );
                layer.down.multiply(cmd, &self.gated, &self.proj, n);
                normalize(&self.proj, &layer.post_ffn, &self.tmp, &self.x, true);
            }
            if readout {
                cmd.dispatch(
                    &k.norm[0],
                    (1, 1),
                    32,
                    &[&self.x, &e.final_norm, &self.x, &self.norm],
                    Some((n - 1) as u32),
                );
                e.head.multiply(cmd, &self.norm, &self.logits, 1);
            }
        })?;
        self.pos += n;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use half::f16;

    #[test]
    fn packed_decode_and_padded_prefill_match_independent_scalar_dot_products() -> Result<()> {
        let gpu = Gpu::open()?;
        let (rows, cols) = (2304, 1536);
        let words: Vec<u32> = (0..(rows * cols / 8) as u32)
            .map(|i| i.wrapping_mul(0x9e3779b9).rotate_right(7))
            .collect();
        let scales: Vec<f16> = (0..rows * cols / 32)
            .map(|i| f16::from_f32(((i % 29) as f32 - 14.) / 100.))
            .collect();
        let matrix = Matrix {
            q: gpu.upload(bytemuck::cast_slice(&words))?,
            scale: gpu.upload(bytemuck::cast_slice(&scales))?,
            rows,
            mv: gpu.pipeline("mv_1536_2304")?,
            mm: Some(gpu.pipeline("mm_1536_2304")?),
        };
        for batch in [1usize, 7, 8, 31, 33] {
            let source: Vec<f32> = (0..cols * batch)
                .map(|i| ((i * 59 % 103) as f32 - 51.) / 71.)
                .collect();
            let x = gpu.upload(bytemuck::cast_slice(&source))?;
            let y = gpu.empty(rows * batch.next_multiple_of(32) * 4)?;
            gpu.submit(|cmd| matrix.multiply(cmd, &x, &y, batch))?;
            let result = y.read_scores();
            for token in 0..batch {
                for row in [0, 7, 15, 31, 63, 64, 129, rows - 1] {
                    let mut expected = 0.;
                    for col in 0..cols {
                        let i = row * cols + col;
                        let code = ((words[i / 8] >> (4 * (i % 8))) & 15) as f32 - 8.;
                        let mut weight = code * scales[i / 32].to_f32();
                        let mut value = source[token * cols + col];
                        if batch >= 8 {
                            weight = f16::from_f32(weight).to_f32();
                            value = f16::from_f32(value).to_f32();
                        }
                        expected += weight * value;
                    }
                    assert!(
                        (result[token * rows + row] - expected).abs() < 0.002,
                        "batch={batch} row={row} token={token} want={expected} got={}",
                        result[token * rows + row]
                    );
                }
            }
        }
        Ok(())
    }

    #[test]
    fn attention_respects_causal_and_sliding_boundaries() -> Result<()> {
        let gpu = Gpu::open()?;
        let n = 3usize;
        let start = 2047usize;
        let q: Vec<f32> = (0..n * 2048)
            .map(|i| ((i * 13 % 97) as f32 - 48.) / 50.)
            .collect();
        // The earliest keys have distinctive values so a shift of the
        // left boundary is observable even in a two-thousand-token window.
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
        let out = gpu.empty(n * 2048 * 4)?;
        for full in [false, true] {
            let state = gpu.pipeline(if full { "attention_1" } else { "attention_0" })?;
            gpu.submit(|cmd| {
                cmd.dispatch(
                    &state,
                    (16, n),
                    256,
                    &[&qq, &kk, &vv, &out],
                    Some(start as u32),
                )
            })?;
            let result = out.read_scores();
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
                                .map(|(&a, b)| f64::from(a) * f64::from(b.to_f32()))
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
                        assert!(
                            (f64::from(result[(16 * t + h) * 128 + col]) - want).abs() < 0.0001,
                            "t={t} full={full} h={h} col={col}"
                        );
                    }
                }
            }
        }
        Ok(())
    }
}
