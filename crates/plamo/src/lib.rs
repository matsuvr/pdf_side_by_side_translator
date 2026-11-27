mod attention;
mod page;
mod model;
mod native;
mod pool;
mod readout;
mod s4;
mod tokenizer;
mod weights;

use pool::Pool;
use std::{io, path::Path};
pub use tokenizer::Tokenizer;
use weights::{Tensor, Weights};

pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
fn err(s: impl Into<String>) -> Box<dyn std::error::Error + Send + Sync> {
    io::Error::new(io::ErrorKind::InvalidData, s.into()).into()
}

const D: usize = 1536;
const H: usize = 128;
const Q: usize = 2048;
const FF: usize = 6144;
const V: usize = 107520;
const L: usize = 16;
const WIN: usize = 2048;
pub(crate) const PREFILL_CHUNK: usize = 128;

struct LayerPlan {
    attn_norm: Vec<f32>,
    attn_q_norm: Vec<f32>,
    attn_k_norm: Vec<f32>,
    post_attention_norm: Vec<f32>,
    ffn_norm: Vec<f32>,
    post_ffw_norm: Vec<f32>,
    attn_qkv: Tensor,
    attn_output: Tensor,
    ffn_up: Tensor,
    ffn_down: Tensor,
}

struct Plan {
    token_embd: Tensor,
    output: Tensor,
    output_norm: Vec<f32>,
    layers: Vec<LayerPlan>,
}

fn clone_tensor(g: &Weights, name: &str) -> Result<Tensor> {
    let t = g
        .tensors
        .get(name)
        .ok_or_else(|| err(format!("missing tensor {name}")))?;
    Ok(Tensor {
        dims: t.dims.clone(),
        ty: t.ty,
        offset: t.offset,
        len: t.len,
    })
}

fn decode_f32_tensor(g: &Weights, name: &str) -> Result<Vec<f32>> {
    let t = g
        .tensors
        .get(name)
        .ok_or_else(|| err(format!("missing tensor {name}")))?;
    let bytes = g.data(t);
    Ok(bytes
        .chunks_exact(4)
        .map(|b| f32::from_le_bytes(b.try_into().unwrap()))
        .collect())
}

impl Plan {
    fn build(g: &Weights) -> Result<Self> {
        let token_embd = clone_tensor(g, "token_embd.weight")?;
        let output = clone_tensor(g, "output.weight")?;
        let output_norm = decode_f32_tensor(g, "output_norm.weight")?;
        let mut layers = Vec::with_capacity(L);
        for i in 0..L {
            layers.push(LayerPlan {
                attn_norm: decode_f32_tensor(g, &format!("blk.{i}.attn_norm.weight"))?,
                attn_q_norm: decode_f32_tensor(g, &format!("blk.{i}.attn_q_norm.weight"))?,
                attn_k_norm: decode_f32_tensor(g, &format!("blk.{i}.attn_k_norm.weight"))?,
                post_attention_norm: decode_f32_tensor(
                    g,
                    &format!("blk.{i}.post_attention_norm"),
                )?,
                ffn_norm: decode_f32_tensor(g, &format!("blk.{i}.ffn_norm.weight"))?,
                post_ffw_norm: decode_f32_tensor(g, &format!("blk.{i}.post_ffw_norm"))?,
                attn_qkv: clone_tensor(g, &format!("blk.{i}.attn_qkv.weight"))?,
                attn_output: clone_tensor(g, &format!("blk.{i}.attn_output.weight"))?,
                ffn_up: clone_tensor(g, &format!("blk.{i}.ffn_up.weight"))?,
                ffn_down: clone_tensor(g, &format!("blk.{i}.ffn_down.weight"))?,
            });
        }
        Ok(Self {
            token_embd,
            output,
            output_norm,
            layers,
        })
    }
}

pub struct Engine {
    g: Weights,
    plan: Plan,
    pool: Pool,
    pub tokenizer: Tokenizer,
    context: usize,
    readout: Option<readout::Readout>,
    fixed_prefixes: std::sync::Mutex<std::collections::HashMap<Vec<u32>, PrefixState>>,
}

// Only the fixed PFML instructions before source content are retained.
// No user document token or translation is written to this cache.
struct PrefixState {
    layers: Vec<(Vec<u16>, Vec<u16>)>,
}

struct LayerCache {
    k: Vec<u16>,
    v: Vec<u16>,
    capacity: usize,
}

impl LayerCache {
    fn new(capacity: usize) -> Self {
        Self {
            k: Vec::new(),
            v: Vec::new(),
            capacity,
        }
    }

    fn append(&mut self, pos: usize, k: &[f32], v: &[f32]) {
        let i = (pos % self.capacity) * H;
        if i == self.k.len() {
            if self.k.capacity() < i + H {
                let target = ((i / H + 1).max(256).next_power_of_two()).min(self.capacity) * H;
                self.k.reserve(target - self.k.len());
                self.v.reserve(target - self.v.len());
            }
            self.k.resize(i + H, 0);
            self.v.resize(i + H, 0);
        }
        attention::f32_slice_to_f16(k, &mut self.k[i..i + H]);
        attention::f32_slice_to_f16(v, &mut self.v[i..i + H]);
    }
}

#[derive(Debug)]
pub struct Translation {
    pub text: String,
    pub tokens: Vec<u32>,
    pub prompt_tokens: usize,
    pub stopped: bool,
    pub prefill_seconds: f64,
    pub decode_seconds: f64,
    pub decode_steps: usize,
    pub drafted_tokens: usize,
    pub accepted_draft_tokens: usize,
    pub target_passes: usize,
}

/// Results remain in request order; each document chunk has an isolated KV cache.
#[derive(Debug)]
pub struct BatchItem {
    pub text: String,
    pub tokens: Vec<u32>,
    pub prompt_tokens: usize,
    pub stopped: bool,
}

#[derive(Debug)]
pub struct BatchTranslation {
    pub items: Vec<BatchItem>,
    pub prefill_seconds: f64,
    pub decode_seconds: f64,
}

#[derive(Clone, Copy)]
struct RopeCoeff {
    sin_s: [f32; H / 2],
    cos_s: [f32; H / 2],
}

pub struct Session<'a> {
    engine: &'a Engine,
    cache: Vec<LayerCache>,
    pos: usize,
    x: Vec<f32>,
    norm: Vec<f32>,
    qkv: Vec<f32>,
    attn: Vec<f32>,
    proj: Vec<f32>,
    ff: Vec<f32>,
    gated: Vec<f32>,
    logits: Vec<f32>,
    scores: Vec<f32>,
    acts: Vec<s4::Act>,
    rope: [Vec<(f32, f32)>; 2],
}

#[derive(Default)]
pub struct TranslationRequest<'a> {
    pub text: &'a str,
    pub source: Option<&'a str>,
    pub target: &'a str,
    pub form: Option<&'a str>,
    pub context: Option<&'a str>,
    pub glossary: &'a [(&'a str, &'a str)],
}

fn validate_text(s: &str) -> Result<()> {
    if s.contains("<|plamo:") {
        Err(err("PFML reserved marker in text"))
    } else {
        Ok(())
    }
}

fn blank_translation_text(text: &str) -> bool {
    text.chars().all(|c| c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c))
}

fn lang(s: &str) -> Result<&str> {
    let s = if s == "ja(easy)" { "ja-easy" } else { s };
    let mut it = s.split('-');
    let a = it.next().unwrap_or("");
    let b = it.next();
    if a.len() < 2
        || a.len() > 3
        || b.is_some_and(|b| b.len() != 4)
        || it.next().is_some()
        || !s.bytes().all(|c| c.is_ascii_lowercase() || c == b'-')
    {
        return Err(err("invalid language code"));
    }
    Ok(s)
}

impl TranslationRequest<'_> {
    pub fn prompt(&self) -> Result<String> {
        validate_text(self.text)?;
        if blank_translation_text(self.text) {
            return Err(err("empty source text"));
        }
        let target = lang(self.target)?;
        let source = self.source.map(lang).transpose()?;
        let mut p = String::from("<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation");
        if let Some(c) = self.context {
            validate_text(c)?;
            if !c.is_empty() {
                p.push_str("<|plamo:tag|>context<|plamo:msg|>");
                p.push_str(c);
            }
        }
        if !self.glossary.is_empty() {
            p.push_str("<|plamo:tag|>glossary");
            for &(s, t) in self.glossary {
                validate_text(s)?;
                validate_text(t)?;
                if blank_translation_text(s) || blank_translation_text(t) {
                    return Err(err("empty glossary term"));
                }
                p.push_str("<|plamo:index|>");
                p.push_str(s);
                p.push_str("<|plamo:item|>");
                p.push_str(t);
            }
        }
        p.push_str("<|plamo:tag|>input");
        if let Some(s) = source {
            p.push_str("<|plamo:key|>lang<|plamo:val|>");
            p.push_str(s);
        }
        p.push_str("<|plamo:msg|>");
        p.push_str(self.text);
        p.push_str("<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>");
        p.push_str(target);
        if let Some(f) = self.form {
            if target != "ja" || !["plain", "polite"].contains(&f) {
                return Err(err("invalid Japanese form"));
            }
            p.push_str("<|plamo:key|>form<|plamo:val|>");
            p.push_str(f);
        }
        p.push_str("<|plamo:msg|>");
        Ok(p)
    }
}

impl Engine {
    pub fn open(path: impl AsRef<Path>, threads: usize, context: usize) -> Result<Self> {
        Self::open_with_exact(path, threads, context, false)
    }

    pub fn open_with_exact(
        path: impl AsRef<Path>,
        threads: usize,
        context: usize,
        exact: bool,
    ) -> Result<Self> {
        Self::from_model(model::open(path.as_ref())?, threads, context, exact)
    }

    fn from_model(g: Weights, threads: usize, context: usize, exact: bool) -> Result<Self> {
        if threads == 0 || threads > 256 || !(1..=32768).contains(&context) {
            return Err(err("threads must be 1..256; context must be 1..32768"));
        }
        let tokenizer = Tokenizer::load(&g)?;
        let plan = Plan::build(&g)?;
        let pool = Pool::new(threads)?;
        let readout = if exact {
            None
        } else {
            let _guard = pool.enter();
            readout::Readout::build_native(&pool, &g, &plan.output)
        };
        Ok(Self {
            g,
            plan,
            pool,
            tokenizer,
            context,
            readout,
            fixed_prefixes: Default::default(),
        })
    }

    #[inline]
    fn multiply(
        &self,
        t: &Tensor,
        x: &[f32],
        out: &mut [f32],
        tokens: usize,
        acts: &mut Vec<s4::Act>,
    ) {
        s4::multiply_tile_pooled(&self.pool, &self.g, t, x, out, tokens, acts);
    }

    /// Developer-only deterministic conversion; never invoked by the application.
    pub fn convert_x86_model(dir: impl AsRef<Path>, output: impl AsRef<Path>) -> Result<()> {
        model::convert(dir.as_ref(), output.as_ref())
    }

    fn prefix_key(&self, r: &TranslationRequest<'_>, tokens: &[u32]) -> Option<Vec<u32>> {
        if r.context.is_some_and(|c| !c.is_empty()) || !r.glossary.is_empty() {
            return None;
        }
        let mut fixed = String::from(
            "<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input",
        );
        if let Some(source) = r.source {
            fixed.push_str("<|plamo:key|>lang<|plamo:val|>");
            fixed.push_str(lang(source).ok()?);
        }
        fixed.push_str("<|plamo:msg|>");
        let key = self.tokenizer.encode(&fixed);
        if key.len() < tokens.len() && tokens.starts_with(&key) {
            Some(key)
        } else {
            None
        }
    }

    fn restore_prefix(&self, s: &mut Session, key: Option<&Vec<u32>>) -> usize {
        let Some(key) = key else {
            return 0;
        };
        let Ok(cache) = self.fixed_prefixes.lock() else {
            return 0;
        };
        let Some(state) = cache.get(key) else {
            return 0;
        };
        for (dst, (k, v)) in s.cache.iter_mut().zip(&state.layers) {
            dst.k.extend_from_slice(k);
            dst.v.extend_from_slice(v);
        }
        s.pos = key.len();
        s.pos
    }

    fn remember_prefix(&self, s: &Session, key: Option<&Vec<u32>>) {
        let Some(key) = key else {
            return;
        };
        if s.pos < key.len()
            || s.cache
                .iter()
                .any(|c| s.pos > c.capacity || c.k.len() < key.len() * H)
        {
            return;
        }
        let Ok(mut cache) = self.fixed_prefixes.lock() else {
            return;
        };
        if cache.contains_key(key) || cache.len() >= 16 {
            return;
        }
        let n = key.len() * H;
        let layers = s
            .cache
            .iter()
            .map(|c| (c.k[..n].to_vec(), c.v[..n].to_vec()))
            .collect();
        cache.insert(key.clone(), PrefixState { layers });
    }

    pub fn session(&self) -> Session<'_> {
        Session {
            engine: self,
            cache: (0..L)
                .map(|i| {
                    LayerCache::new(if i % 8 == 7 {
                        self.context
                    } else {
                        self.context.min(WIN)
                    })
                })
                .collect(),
            pos: 0,
            x: vec![0.; D],
            norm: vec![0.; D],
            qkv: vec![0.; Q + 2 * H],
            attn: vec![0.; Q],
            proj: vec![0.; D],
            ff: vec![0.; 2 * FF],
            gated: vec![0.; FF],
            logits: Vec::new(),
            scores: Vec::new(),
            acts: Vec::with_capacity(FF / 256),
            rope: [rotary(false), rotary(true)],
        }
    }

    pub fn translate(&self, r: &TranslationRequest<'_>, max_new_tokens: usize) -> Result<String> {
        Ok(self.translate_with_stats(r, max_new_tokens)?.text)
    }

    pub fn translate_with_stats(
        &self,
        r: &TranslationRequest<'_>,
        max_new_tokens: usize,
    ) -> Result<Translation> {
        self.translate_with_options(r, max_new_tokens, true)
    }

    /// Speculation is a verified n-gram lookup; disable for reproducible baseline measurements.
    pub fn translate_with_options(
        &self,
        r: &TranslationRequest<'_>,
        max_new_tokens: usize,
        speculate: bool,
    ) -> Result<Translation> {
        let tokens = self.tokenizer.encode(&r.prompt()?);
        if max_new_tokens == 0
            || tokens.is_empty()
            || tokens
                .len()
                .checked_add(max_new_tokens)
                .is_none_or(|n| n > self.context)
        {
            return Err(err(
                "invalid output limit or input plus output exceeds configured context",
            ));
        }
        let _guard = self.pool.enter();
        let start = std::time::Instant::now();
        let mut s = self.session();
        let prefix = self.prefix_key(r, &tokens);
        let reused = self.restore_prefix(&mut s, prefix.as_ref());
        for chunk in tokens[reused..].chunks(PREFILL_CHUNK) {
            s.forward_batch(chunk)?;
        }
        if reused == 0 {
            self.remember_prefix(&s, prefix.as_ref());
        }
        let mut t = s.readout(true)?;
        let prefill_seconds = start.elapsed().as_secs_f64();
        let start = std::time::Instant::now();
        let mut out = Vec::new();
        let mut stopped = false;
        let mut decode_steps = 0;
        let mut drafted_tokens = 0;
        let mut accepted_draft_tokens = 0;
        let mut target_passes = 0;
        let mut draft_cap = 6usize;
        let mut misses = 0usize;
        while out.len() < max_new_tokens {
            if [1, 2, 16].contains(&t) {
                stopped = true;
                break;
            }
            out.push(t);
            if out.len() >= max_new_tokens {
                break;
            }
            // Never overwrite a sliding-window entry which could be required after rollback.
            let limit = max_new_tokens - out.len();
            let draft = if speculate && misses < 2 && s.pos + 2 < WIN.min(self.context) {
                ngram_draft(
                    &out,
                    limit.min(draft_cap).min(WIN.min(self.context) - s.pos - 1),
                )
            } else {
                Vec::new()
            };
            if draft.is_empty() {
                t = s.forward(t, true, true)?.ok_or_else(|| err("no logits"))?;
                decode_steps += 1;
                target_passes += 1;
                continue;
            }
            let old = s.pos;
            let mut items = Vec::with_capacity(draft.len() + 1);
            items.push((0, t));
            items.extend(draft.iter().map(|&d| (0, d)));
            let hidden = self.forward_group(&mut [&mut s], &items)?;
            let verified = self.hidden_readouts(&hidden)?;
            drafted_tokens += draft.len();
            let mut accepted = 0;
            for (&proposed, &v) in draft.iter().zip(&verified) {
                if proposed != v || [1, 2, 16].contains(&v) {
                    break;
                }
                out.push(v);
                accepted += 1;
            }
            accepted_draft_tokens += accepted;
            decode_steps += 1 + accepted;
            target_passes += 1;
            if accepted == 0 {
                misses += 1;
                draft_cap = 3;
            } else if accepted * 2 >= draft.len() {
                misses = 0;
                draft_cap = (draft_cap + 3).min(12);
            } else {
                draft_cap = 3;
            }

            let keep = old + 1 + accepted;
            for c in &mut s.cache {
                c.k.truncate(keep * H);
                c.v.truncate(keep * H);
            }
            s.pos = keep;
            s.x.copy_from_slice(&hidden[accepted * D..(accepted + 1) * D]);
            t = verified[accepted];
        }
        let decode_seconds = start.elapsed().as_secs_f64();
        Ok(Translation {
            text: self.tokenizer.decode(&out)?,
            tokens: out,
            prompt_tokens: tokens.len(),
            stopped,
            prefill_seconds,
            decode_seconds,
            decode_steps,
            drafted_tokens,
            accepted_draft_tokens,
            target_passes,
        })
    }

    /// Process up to 8 independent translations together, amortizing the weight reads.
    /// All requests retain their own token stream, positions and KV cache.
    pub fn translate_batch(
        &self,
        requests: &[TranslationRequest<'_>],
        max_new_tokens: usize,
    ) -> Result<BatchTranslation> {
        if requests.is_empty() || requests.len() > 8 || max_new_tokens == 0 {
            return Err(err(
                "batch size must be 1..8; nonzero output limit required",
            ));
        }
        let prompts = requests
            .iter()
            .map(|r| Ok(self.tokenizer.encode(&r.prompt()?)))
            .collect::<Result<Vec<_>>>()?;
        if prompts.iter().any(|p| {
            p.is_empty()
                || p.len()
                    .checked_add(max_new_tokens)
                    .is_none_or(|n| n > self.context)
        }) {
            return Err(err("batch context limit"));
        }
        let _guard = self.pool.enter();
        let start = std::time::Instant::now();
        let mut sessions = requests.iter().map(|_| self.session()).collect::<Vec<_>>();
        let keys = requests
            .iter()
            .zip(&prompts)
            .map(|(r, p)| self.prefix_key(r, p))
            .collect::<Vec<_>>();
        let mut reused = sessions
            .iter_mut()
            .zip(&keys)
            .map(|(s, k)| self.restore_prefix(s, k.as_ref()))
            .collect::<Vec<_>>();
        // Cold batch: identical fixed instructions need only one execution.
        for i in 0..sessions.len() {
            if reused[i] != 0 {
                continue;
            }
            let Some(key) = keys[i].as_ref() else {
                continue;
            };
            if keys.iter().filter(|k| k.as_ref() == Some(key)).count() < 2 {
                continue;
            }
            let mut seed = self.session();
            seed.forward_batch(key)?;
            self.remember_prefix(&seed, Some(key));
            for j in 0..sessions.len() {
                if reused[j] == 0 && keys[j].as_ref() == Some(key) {
                    reused[j] = self.restore_prefix(&mut sessions[j], Some(key));
                }
            }
        }
        let all = prompts
            .iter()
            .enumerate()
            .flat_map(|(i, p)| p[reused[i]..].iter().map(move |&t| (i, t)))
            .collect::<Vec<_>>();
        for chunk in all.chunks(PREFILL_CHUNK) {
            let mut refs = sessions.iter_mut().collect::<Vec<_>>();
            self.forward_group(&mut refs, chunk)?;
        }
        for (i, s) in sessions.iter().enumerate() {
            if reused[i] == 0 {
                self.remember_prefix(s, keys[i].as_ref());
            }
        }
        let mut next =
            self.readout_group(&mut sessions, &(0..requests.len()).collect::<Vec<_>>())?;
        let prefill_seconds = start.elapsed().as_secs_f64();
        let start = std::time::Instant::now();
        let mut output = vec![Vec::new(); requests.len()];
        let mut stopped = vec![false; requests.len()];
        for _ in 0..max_new_tokens {
            let mut active = Vec::new();
            for (i, n) in next.iter().copied().enumerate() {
                if stopped[i] || output[i].len() >= max_new_tokens {
                    continue;
                }
                if [1, 2, 16].contains(&n) {
                    stopped[i] = true;
                    continue;
                }
                output[i].push(n);
                if output[i].len() < max_new_tokens {
                    active.push((i, n));
                }
            }
            if active.is_empty() {
                break;
            }
            if active.len() == 1 {
                let (i, t) = active[0];
                next[i] = sessions[i]
                    .forward(t, true, true)?
                    .ok_or_else(|| err("no logits"))?;
                continue;
            }
            let ids = active.iter().map(|&(i, _)| i).collect::<Vec<_>>();
            let mut refs = sessions.iter_mut().collect::<Vec<_>>();
            self.forward_group(&mut refs, &active)?;
            for (&i, t) in ids.iter().zip(self.readout_group(&mut sessions, &ids)?) {
                next[i] = t;
            }
        }
        let decode_seconds = start.elapsed().as_secs_f64();
        let items = output
            .into_iter()
            .zip(stopped)
            .zip(prompts)
            .map(|((tokens, stopped), prompt)| {
                Ok(BatchItem {
                    text: self.tokenizer.decode(&tokens)?,
                    tokens,
                    prompt_tokens: prompt.len(),
                    stopped,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(BatchTranslation {
            items,
            prefill_seconds,
            decode_seconds,
        })
    }

    fn hidden_readouts(&self, hidden: &[f32]) -> Result<Vec<u32>> {
        let count = hidden.len() / D;
        let g = &self.g;
        let out = &self.plan.output;
        let w = &self.plan.output_norm;
        let mut norms = vec![0.; hidden.len()];
        for (x, n) in hidden.chunks_exact(D).zip(norms.chunks_exact_mut(D)) {
            rms(x, w, n);
        }
        if norms.iter().any(|v| !v.is_finite()) {
            return Err(err("non-finite activation"));
        }
        let ids: Vec<u32> = if let Some(index) = &self.readout {
            norms
                .chunks_exact(D)
                .map(|x| index.best_native(&self.pool, g, out, x))
                .collect()
        } else {
            let mut scores = vec![0.; V * count];
            native::multiply_tile8_pooled(&self.pool, g, out, &norms, &mut scores, count);
            scores
                .chunks_exact(V)
                .map(|row| {
                    row.iter()
                        .enumerate()
                        .max_by(|a, b| a.1.total_cmp(b.1).then(b.0.cmp(&a.0)))
                        .unwrap()
                        .0 as u32
                })
                .collect()
        };
        if ids.iter().any(|&id| id as usize >= V) {
            return Err(err("invalid output token"));
        }
        Ok(ids)
    }

    fn readout_group(&self, sessions: &mut [Session], selected: &[usize]) -> Result<Vec<u32>> {
        let g = &self.g;
        let output = &self.plan.output;
        let w = &self.plan.output_norm;
        let mut norms = Vec::with_capacity(D * selected.len());
        for &i in selected {
            let s = &mut sessions[i];
            rms(&s.x, w, &mut s.norm);
            if s.norm.iter().any(|v| !v.is_finite()) {
                return Err(err("non-finite activation"));
            }
            norms.extend_from_slice(&s.norm);
        }
        let ids: Vec<u32> = if let Some(index) = &self.readout {
            norms
                .chunks_exact(D)
                .map(|x| index.best_native(&self.pool, g, output, x))
                .collect()
        } else {
            let mut scores = vec![0.; V * selected.len()];
            native::multiply_tile8_pooled(&self.pool, g, output, &norms, &mut scores, selected.len());
            scores
                .chunks_exact(V)
                .map(|row| {
                    row.iter()
                        .enumerate()
                        .max_by(|a, b| a.1.total_cmp(b.1).then(b.0.cmp(&a.0)))
                        .unwrap()
                        .0 as u32
                })
                .collect()
        };
        if ids.iter().any(|&id| id as usize >= V) {
            return Err(err("invalid output token"));
        }
        Ok(ids)
    }

    fn forward_group(
        &self,
        sessions: &mut [&mut Session],
        items: &[(usize, u32)],
    ) -> Result<Vec<f32>> {
        let g = &self.g;
        let plan = &self.plan;
        let b = items.len();
        let mut counts = vec![0usize; sessions.len()];
        let mut positions = Vec::with_capacity(b);
        let mut coeff = Vec::with_capacity(b);
        for &(which, token) in items {
            let session = sessions.get(which).ok_or_else(|| err("bad batch route"))?;
            let pos = session.pos + counts[which];
            if pos >= self.context || token as usize >= V {
                return Err(err("batch context or token limit"));
            }
            positions.push(pos);
            counts[which] += 1;
            coeff.push([
                rope_coeff(pos, &session.rope[0]),
                rope_coeff(pos, &session.rope[1]),
            ]);
        }
        let mut x = vec![0.0f32; b * D];
        for (j, &(_, t)) in items.iter().enumerate() {
            native::row_into(g, &plan.token_embd, t as usize, &mut x[j * D..(j + 1) * D])?;
        }
        let mut norm = vec![0.; b * D];
        let mut qkv = vec![0.; b * (Q + 2 * H)];
        let mut attn = vec![0.; b * Q];
        let mut proj = vec![0.; b * D];
        let mut ff = vec![0.; b * 2 * FF];
        let mut gated = vec![0.; b * FF];
        let mut batch_acts = Vec::with_capacity(b * (FF / 256));

        for i in 0..L {
            let lp = &plan.layers[i];
            let full = i % 8 == 7;
            for (src, dst) in x.chunks_exact(D).zip(norm.chunks_exact_mut(D)) {
                rms(src, &lp.attn_norm, dst);
            }
            self.multiply(&lp.attn_qkv, &norm, &mut qkv, b, &mut batch_acts);
            let use_prefill =
                sessions.len() == 1 && b >= 4 && b <= sessions[0].cache[i].capacity;
            let evicted = use_prefill
                .then(|| attention::EvictedRows::capture(&sessions[0].cache[i], &positions));
            for (j, (row, out)) in qkv
                .chunks_exact_mut(Q + 2 * H)
                .zip(attn.chunks_exact_mut(Q))
                .enumerate()
            {
                let pos = positions[j];
                let session = &mut sessions[items[j].0];
                let (q, kv) = row.split_at_mut(Q);
                let (k, v) = kv.split_at_mut(H);
                let table = &coeff[j][usize::from(full)];
                for qh in q.chunks_exact_mut(H) {
                    rms_inplace(qh, &lp.attn_q_norm);
                    rope(qh, table);
                }
                rms_inplace(k, &lp.attn_k_norm);
                rope(k, table);
                let cache = &mut session.cache[i];
                cache.append(pos, k, v);
                if evicted.is_none() {
                    attention::single(&self.pool, q, cache, pos, out, &mut session.scores);
                }
            }
            if let Some(evicted) = evicted {
                let session = &mut sessions[0];
                attention::prefill(
                    &self.pool,
                    &qkv,
                    &session.cache[i],
                    &positions,
                    &evicted,
                    &mut attn,
                );
            }
            self.multiply(&lp.attn_output, &attn, &mut proj, b, &mut batch_acts);
            for row in proj.chunks_exact_mut(D) {
                rms_inplace(row, &lp.post_attention_norm);
            }
            add_inplace(&mut x, &proj);
            for (src, dst) in x.chunks_exact(D).zip(norm.chunks_exact_mut(D)) {
                rms(src, &lp.ffn_norm, dst);
            }
            self.multiply(&lp.ffn_up, &norm, &mut ff, b, &mut batch_acts);
            if b >= 8 && self.pool.num_threads() > 1 {
                let ff_ref: &[f32] = &ff;
                let gated_addr = gated.as_mut_ptr() as usize;
                let nth = self.pool.num_threads().min(b);
                self.pool.dispatch(nth, |ith, nth| {
                    let r_start = ith * b / nth;
                    let r_end = (ith + 1) * b / nth;
                    for r in r_start..r_end {
                        let src = &ff_ref[r * 2 * FF..(r + 1) * 2 * FF];
                        let dst = unsafe {
                            std::slice::from_raw_parts_mut(
                                (gated_addr as *mut f32).add(r * FF),
                                FF,
                            )
                        };
                        swiglu(src, dst);
                    }
                });
            } else {
                for (src, dst) in ff.chunks_exact(2 * FF).zip(gated.chunks_exact_mut(FF)) {
                    swiglu(src, dst);
                }
            }
            self.multiply(&lp.ffn_down, &gated, &mut proj, b, &mut batch_acts);
            for row in proj.chunks_exact_mut(D) {
                rms_inplace(row, &lp.post_ffw_norm);
            }
            add_inplace(&mut x, &proj);
        }
        for (j, &(which, _)) in items.iter().enumerate() {
            sessions[which].x.copy_from_slice(&x[j * D..(j + 1) * D]);
        }
        for (session, count) in sessions.iter_mut().zip(counts) {
            session.pos += count;
        }
        Ok(x)
    }
}

/// Find a previous exact 2..4-token suffix and draft only already emitted tokens.
/// Verification guarantees the same greedy output as the ordinary path.
fn ngram_draft(history: &[u32], limit: usize) -> Vec<u32> {
    if limit == 0 || history.len() < 7 {
        return Vec::new();
    }
    for n in (3..=4.min(history.len() / 2)).rev() {
        let suffix = &history[history.len() - n..];
        for p in (0..history.len() - n).rev() {
            if p + n >= history.len() || &history[p..p + n] != suffix {
                continue;
            }
            let end = (p + n + limit).min(history.len() - n);
            if end > p + n {
                return history[p + n..end].to_vec();
            }
        }
    }
    Vec::new()
}

fn rotary(full: bool) -> Vec<(f32, f32)> {
    let base = if full { 1e6f32 } else { 1e4 };
    let (lo, hi) = if full {
        let f = |beta: f32| {
            H as f32 / 2. * (4096. / (beta * 2. * std::f32::consts::PI)).ln() / base.ln()
        };
        (f(32.), f(1.))
    } else {
        (0., 1.)
    };
    (0..H / 2)
        .map(|i| {
            let freq = base.powf(-((2 * i) as f32) / H as f32);
            if full {
                let ramp = ((i as f32 - lo) / (hi - lo)).clamp(0., 1.);
                (freq * (1. - ramp + ramp / 256.), 1. + 0.1 * 256f32.ln())
            } else {
                (freq, 1.)
            }
        })
        .collect()
}

fn rope_coeff(pos: usize, table: &[(f32, f32)]) -> RopeCoeff {
    let mut sin_s = [0.0f32; H / 2];
    let mut cos_s = [0.0f32; H / 2];
    let p = pos as f32;
    for i in 0..H / 2 {
        let (freq, scale) = table[i];
        let (s, c) = (p * freq).sin_cos();
        sin_s[i] = s * scale;
        cos_s[i] = c * scale;
    }
    RopeCoeff { sin_s, cos_s }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn rope_avx2(x: &mut [f32], coeff: &RopeCoeff) {
    use std::arch::x86_64::*;
    let xp = x.as_mut_ptr();
    let sp = coeff.sin_s.as_ptr();
    let cp = coeff.cos_s.as_ptr();
    unsafe {
        for i in (0..H / 2).step_by(8) {
            let a = _mm256_loadu_ps(xp.add(i));
            let b = _mm256_loadu_ps(xp.add(H / 2 + i));
            let s = _mm256_loadu_ps(sp.add(i));
            let c = _mm256_loadu_ps(cp.add(i));
            let na = _mm256_fmsub_ps(a, c, _mm256_mul_ps(b, s));
            let nb = _mm256_fmadd_ps(b, c, _mm256_mul_ps(a, s));
            _mm256_storeu_ps(xp.add(i), na);
            _mm256_storeu_ps(xp.add(H / 2 + i), nb);
        }
    }
}

#[inline]
fn rope(x: &mut [f32], coeff: &RopeCoeff) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") && std::is_x86_feature_detected!("fma") {
        unsafe { rope_avx2(x, coeff) };
        return;
    }
    for i in 0..H / 2 {
        let (a, b) = (x[i], x[i + H / 2]);
        let (s, c) = (coeff.sin_s[i], coeff.cos_s[i]);
        x[i] = a * c - b * s;
        x[i + H / 2] = b * c + a * s;
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn swiglu_avx_fma(src: &[f32], dst: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let n = dst.len();
        let one = _mm256_set1_ps(1.);
        let log2e = _mm256_set1_ps(std::f32::consts::LOG2_E);
        let upper = _mm256_set1_ps(80.);
        let lower = _mm256_set1_ps(-80.);
        let ln2_hi = _mm256_set1_ps(0.693359375);
        let ln2_lo = _mm256_set1_ps(-0.00021219444);
        let c7 = _mm256_set1_ps(1. / 5040.);
        let c6 = _mm256_set1_ps(1. / 720.);
        let c5 = _mm256_set1_ps(1. / 120.);
        let c4 = _mm256_set1_ps(1. / 24.);
        let c3 = _mm256_set1_ps(1. / 6.);
        let c2 = _mm256_set1_ps(0.5);
        let c127 = _mm256_set1_epi32(127);
        for i in (0..n).step_by(8) {
            let a = _mm256_loadu_ps(src.as_ptr().add(i));
            let u = _mm256_loadu_ps(src.as_ptr().add(n + i));
            let x = _mm256_max_ps(
                lower,
                _mm256_min_ps(upper, _mm256_sub_ps(_mm256_setzero_ps(), a)),
            );
            let power = _mm256_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(
                _mm256_mul_ps(x, log2e),
            );
            let r = _mm256_fnmadd_ps(power, ln2_lo, _mm256_fnmadd_ps(power, ln2_hi, x));
            let mut p = _mm256_fmadd_ps(c7, r, c6);
            p = _mm256_fmadd_ps(p, r, c5);
            p = _mm256_fmadd_ps(p, r, c4);
            p = _mm256_fmadd_ps(p, r, c3);
            p = _mm256_fmadd_ps(p, r, c2);
            p = _mm256_fmadd_ps(p, r, one);
            p = _mm256_fmadd_ps(p, r, one);
            let e = _mm256_castsi256_ps(_mm256_slli_epi32::<23>(_mm256_add_epi32(
                _mm256_cvttps_epi32(power),
                c127,
            )));
            let ex = _mm256_mul_ps(p, e);
            let result = _mm256_mul_ps(_mm256_div_ps(a, _mm256_add_ps(one, ex)), u);
            _mm256_storeu_ps(dst.as_mut_ptr().add(i), result);
        }
    }
}

fn swiglu(src: &[f32], dst: &mut [f32]) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("fma")
        && dst.len() % 8 == 0
    {
        unsafe { swiglu_avx_fma(src, dst) };
        return;
    }
    let n = dst.len();
    for i in 0..n {
        let a = src[i];
        dst[i] = a / (1. + (-a).exp()) * src[n + i];
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn rms_avx2(x: &[f32], w: &[f32], o: &mut [f32]) {
    use std::arch::x86_64::*;
    let n = x.len();
    unsafe {
        let xp = x.as_ptr();
        let mut s0 = _mm256_setzero_ps();
        let mut s1 = _mm256_setzero_ps();
        let mut s2 = _mm256_setzero_ps();
        let mut s3 = _mm256_setzero_ps();
        for i in (0..n).step_by(32) {
            let v0 = _mm256_loadu_ps(xp.add(i));
            let v1 = _mm256_loadu_ps(xp.add(i + 8));
            let v2 = _mm256_loadu_ps(xp.add(i + 16));
            let v3 = _mm256_loadu_ps(xp.add(i + 24));
            s0 = _mm256_fmadd_ps(v0, v0, s0);
            s1 = _mm256_fmadd_ps(v1, v1, s1);
            s2 = _mm256_fmadd_ps(v2, v2, s2);
            s3 = _mm256_fmadd_ps(v3, v3, s3);
        }
        let sv = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
        let lo = _mm256_castps256_ps128(sv);
        let hi = _mm256_extractf128_ps::<1>(sv);
        let s128 = _mm_add_ps(lo, hi);
        let s128 = _mm_add_ps(s128, _mm_movehl_ps(s128, s128));
        let s128 = _mm_add_ss(s128, _mm_shuffle_ps::<1>(s128, s128));
        let inv = (_mm_cvtss_f32(s128) / n as f32 + 1e-6).sqrt().recip();
        let vinv = _mm256_set1_ps(inv);
        let wp = w.as_ptr();
        let op = o.as_mut_ptr();
        for i in (0..n).step_by(8) {
            let vx = _mm256_loadu_ps(xp.add(i));
            let vw = _mm256_loadu_ps(wp.add(i));
            _mm256_storeu_ps(op.add(i), _mm256_mul_ps(_mm256_mul_ps(vx, vinv), vw));
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn rms_inplace_avx2(x: &mut [f32], w: &[f32]) {
    use std::arch::x86_64::*;
    let n = x.len();
    unsafe {
        let xp = x.as_mut_ptr();
        let mut s0 = _mm256_setzero_ps();
        let mut s1 = _mm256_setzero_ps();
        let mut s2 = _mm256_setzero_ps();
        let mut s3 = _mm256_setzero_ps();
        for i in (0..n).step_by(32) {
            let v0 = _mm256_loadu_ps(xp.add(i));
            let v1 = _mm256_loadu_ps(xp.add(i + 8));
            let v2 = _mm256_loadu_ps(xp.add(i + 16));
            let v3 = _mm256_loadu_ps(xp.add(i + 24));
            s0 = _mm256_fmadd_ps(v0, v0, s0);
            s1 = _mm256_fmadd_ps(v1, v1, s1);
            s2 = _mm256_fmadd_ps(v2, v2, s2);
            s3 = _mm256_fmadd_ps(v3, v3, s3);
        }
        let sv = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
        let lo = _mm256_castps256_ps128(sv);
        let hi = _mm256_extractf128_ps::<1>(sv);
        let s128 = _mm_add_ps(lo, hi);
        let s128 = _mm_add_ps(s128, _mm_movehl_ps(s128, s128));
        let s128 = _mm_add_ss(s128, _mm_shuffle_ps::<1>(s128, s128));
        let inv = (_mm_cvtss_f32(s128) / n as f32 + 1e-6).sqrt().recip();
        let vinv = _mm256_set1_ps(inv);
        let wp = w.as_ptr();
        for i in (0..n).step_by(8) {
            let vx = _mm256_loadu_ps(xp.add(i));
            let vw = _mm256_loadu_ps(wp.add(i));
            _mm256_storeu_ps(xp.add(i), _mm256_mul_ps(vx, _mm256_mul_ps(vinv, vw)));
        }
    }
}

#[inline]
fn rms(x: &[f32], w: &[f32], o: &mut [f32]) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("fma")
        && x.len() % 32 == 0
    {
        unsafe { rms_avx2(x, w, o) };
        return;
    }
    let inv = (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32 + 1e-6)
        .sqrt()
        .recip();
    for ((o, &x), &w) in o.iter_mut().zip(x).zip(w) {
        *o = x * inv * w;
    }
}

#[inline]
fn rms_inplace(x: &mut [f32], w: &[f32]) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2")
        && std::is_x86_feature_detected!("fma")
        && x.len() % 32 == 0
    {
        unsafe { rms_inplace_avx2(x, w) };
        return;
    }
    let inv = (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32 + 1e-6)
        .sqrt()
        .recip();
    for (x, &w) in x.iter_mut().zip(w) {
        *x *= inv * w;
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn add_inplace_avx2(x: &mut [f32], p: &[f32]) {
    use std::arch::x86_64::*;
    let n = x.len();
    unsafe {
        let xp = x.as_mut_ptr();
        let pp = p.as_ptr();
        for i in (0..n).step_by(8) {
            _mm256_storeu_ps(
                xp.add(i),
                _mm256_add_ps(_mm256_loadu_ps(xp.add(i)), _mm256_loadu_ps(pp.add(i))),
            );
        }
    }
}

#[inline]
fn add_inplace(x: &mut [f32], p: &[f32]) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") && x.len() % 8 == 0 {
        unsafe { add_inplace_avx2(x, p) };
        return;
    }
    for (x, &p) in x.iter_mut().zip(p) {
        *x += p;
    }
}

impl Session<'_> {
    pub fn position(&self) -> usize {
        self.pos
    }

    pub fn logits(&self) -> &[f32] {
        &self.logits
    }

    pub fn step(&mut self, token: u32, readout: bool) -> Result<Option<u32>> {
        if self.pos >= self.engine.context || token as usize >= V {
            return Err(err("context or token limit"));
        }
        let _guard = self.engine.pool.enter();
        self.forward(token, readout, false)
    }

    /// Process prompt in bounded blocks and return the next greedy token.
    pub fn prefill(&mut self, tokens: &[u32]) -> Result<u32> {
        self.prefill_internal(tokens, false)
    }

    fn prefill_internal(&mut self, tokens: &[u32], fast: bool) -> Result<u32> {
        if tokens.is_empty()
            || tokens.iter().any(|&t| t as usize >= V)
            || self
                .pos
                .checked_add(tokens.len())
                .is_none_or(|n| n > self.engine.context)
        {
            return Err(err("context or token limit"));
        }
        let _guard = self.engine.pool.enter();
        for chunk in tokens.chunks(PREFILL_CHUNK) {
            self.forward_batch(chunk)?;
        }
        self.readout(fast)
    }

    fn readout(&mut self, fast: bool) -> Result<u32> {
        let g = &self.engine.g;
        let plan = &self.engine.plan;
        rms(&self.x, &plan.output_norm, &mut self.norm);
        if self.norm.iter().any(|v| !v.is_finite()) {
            return Err(err("non-finite output activation"));
        }
        if fast {
            if let Some(index) = &self.engine.readout {
                let id = index.best_native(&self.engine.pool, g, &plan.output, &self.norm);
                if (id as usize) >= V {
                    return Err(err("invalid output token"));
                }
                return Ok(id);
            }
        }
        if self.logits.len() != V {
            self.logits.resize(V, 0.0);
        }
        native::multiply_tile8_pooled(
            &self.engine.pool,
            g,
            &plan.output,
            &self.norm,
            &mut self.logits,
            1,
        );
        if self.logits.iter().any(|x| !x.is_finite()) {
            return Err(err("non-finite logits"));
        }
        let mut best = 0;
        for i in 1..V {
            if self.logits[i] > self.logits[best] {
                best = i;
            }
        }
        Ok(best as u32)
    }

    fn forward_batch(&mut self, tokens: &[u32]) -> Result<()> {
        let items = tokens.iter().map(|&t| (0, t)).collect::<Vec<_>>();
        let engine = self.engine;
        engine.forward_group(&mut [self], &items).map(|_| ())
    }

    fn forward(&mut self, token: u32, readout: bool, fast: bool) -> Result<Option<u32>> {
        let e = self.engine;
        let g = &e.g;
        let plan = &e.plan;
        native::row_into(g, &plan.token_embd, token as usize, &mut self.x)?;
        let coeff = [
            rope_coeff(self.pos, &self.rope[0]),
            rope_coeff(self.pos, &self.rope[1]),
        ];
        for i in 0..L {
            let lp = &plan.layers[i];
            let full = i % 8 == 7;
            rms(&self.x, &lp.attn_norm, &mut self.norm);
            e.multiply(&lp.attn_qkv, &self.norm, &mut self.qkv, 1, &mut self.acts);
            let (q, kv) = self.qkv.split_at_mut(Q);
            let (k, v) = kv.split_at_mut(H);
            let table = &coeff[usize::from(full)];
            for qh in q.chunks_exact_mut(H) {
                rms_inplace(qh, &lp.attn_q_norm);
                rope(qh, table);
            }
            rms_inplace(k, &lp.attn_k_norm);
            rope(k, table);
            let cache = &mut self.cache[i];
            cache.append(self.pos, k, v);
            attention::single(&e.pool, q, cache, self.pos, &mut self.attn, &mut self.scores);
            e.multiply(
                &lp.attn_output,
                &self.attn,
                &mut self.proj,
                1,
                &mut self.acts,
            );
            rms_inplace(&mut self.proj, &lp.post_attention_norm);
            add_inplace(&mut self.x, &self.proj);
            rms(&self.x, &lp.ffn_norm, &mut self.norm);
            e.multiply(&lp.ffn_up, &self.norm, &mut self.ff, 1, &mut self.acts);
            swiglu(&self.ff, &mut self.gated);
            e.multiply(
                &lp.ffn_down,
                &self.gated,
                &mut self.proj,
                1,
                &mut self.acts,
            );
            rms_inplace(&mut self.proj, &lp.post_ffw_norm);
            add_inplace(&mut self.x, &self.proj);
        }
        self.pos += 1;
        if !readout {
            return Ok(None);
        }
        Ok(Some(self.readout(fast)?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn swiglu_accuracy() {
        let a: Vec<f32> = (0..128).map(|i| i as f32 / 4. - 16.).collect();
        let mut both = a.clone();
        both.extend((0..128).map(|i| (i as f32 * 0.43).cos()));
        let mut got = vec![0.; 128];
        swiglu(&both, &mut got);
        for i in 0..128 {
            let expect = a[i] / (1. + (-a[i]).exp()) * both[128 + i];
            assert!((got[i] - expect).abs() < 2e-6 * (expect.abs() + 1.));
        }
    }

    #[test]
    fn template() {
        let r = TranslationRequest {
            text: "Hello",
            source: Some("en"),
            target: "ja",
            ..Default::default()
        };
        assert_eq!(
            r.prompt().unwrap(),
            "<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>Hello<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja<|plamo:msg|>"
        );
    }

    #[test]
    fn reserved() {
        assert!(
            TranslationRequest {
                text: "<|plamo:tag|>",
                target: "ja",
                ..Default::default()
            }
            .prompt()
            .is_err()
        );
    }

    #[test]
    fn cache_ring() {
        let mut c = LayerCache::new(2);
        for i in 0..3 {
            c.append(i, &[i as f32; H], &[1.; H]);
        }
        assert_eq!(c.k.len(), 2 * H);
        assert_eq!(attention::f16_to_f32_scalar(c.k[0]), 2.);
        assert_eq!(attention::f16_to_f32_scalar(c.k[H]), 1.);
    }
}
