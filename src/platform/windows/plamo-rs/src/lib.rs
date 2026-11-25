mod ffi;
mod weights;
mod model;
mod native;
mod s4;
mod readout;
mod tokenizer;
use weights::{Weights, Tensor};
use rayon::{ThreadPool, ThreadPoolBuilder};
use std::{io, path::Path};
pub use tokenizer::Tokenizer;

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
pub struct Engine {
    g: Weights,
    pool: ThreadPool,
    pub tokenizer: Tokenizer,
    context: usize,
    readout: Option<readout::Readout>,
    fixed_prefixes: std::sync::Mutex<std::collections::HashMap<Vec<u32>, PrefixState>>,
}
// Only the fixed PFML instructions before source content are retained.
// No user document token or translation is written to this cache.
struct PrefixState {
    layers: Vec<(Vec<f32>, Vec<f32>)>,
}
struct LayerCache {
    k: Vec<f32>,
    v: Vec<f32>,
    capacity: usize,
}
impl LayerCache {
    fn new(capacity: usize) -> Self {
        Self {
            k: Vec::with_capacity(capacity * H),
            v: Vec::with_capacity(capacity * H),
            capacity,
        }
    }
    fn append(&mut self, pos: usize, k: &[f32], v: &[f32]) {
        let i = (pos % self.capacity) * H;
        if i == self.k.len() {
            self.k.extend_from_slice(k);
            self.v.extend_from_slice(v);
        } else {
            self.k[i..i + H].copy_from_slice(k);
            self.v[i..i + H].copy_from_slice(v);
        }
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
        if self.text.trim().is_empty() {
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
                if s.trim().is_empty() || t.trim().is_empty() {
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
        let pool = ThreadPoolBuilder::new().num_threads(threads).build()?;
        let readout = if exact {
            None
        } else {
            pool.install(|| readout::Readout::build_native(&g,&g.tensors["output.weight"]))
        };
        Ok(Self {
            g,
            pool,
            tokenizer,
            context,
            readout,
            fixed_prefixes: Default::default(),
        })
    }
    fn multiply(&self, t: &Tensor, x: &[f32], out: &mut [f32], tokens: usize) {
        // This reader validates the complete fixed v7 tensor inventory.
        s4::multiply_tile(&self.g,t,x,out,tokens);
    }
    /// Developer-only deterministic conversion; never invoked by the application.
    pub fn convert_x86_model(dir: impl AsRef<Path>,output:impl AsRef<Path>)->Result<()> {
        model::convert(dir.as_ref(),output.as_ref())
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
            logits: vec![0.; V],
            scores: vec![0.; self.context],
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
        self.pool.install(|| {
            let start = std::time::Instant::now();
            let mut s = self.session();
            let prefix = self.prefix_key(r, &tokens);
            let reused = self.restore_prefix(&mut s, prefix.as_ref());
            for chunk in tokens[reused..].chunks(32) {
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
        self.pool.install(|| {
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
            for chunk in all.chunks(32) {
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
        })
    }
    fn hidden_readouts(&self, hidden: &[f32]) -> Result<Vec<u32>> {
        let count = hidden.len() / D;
        let g = &self.g;
        let out = &g.tensors["output.weight"];
        let w = g.data(&g.tensors["output_norm.weight"]);
        let mut norms = vec![0.; hidden.len()];
        for (x, n) in hidden.chunks_exact(D).zip(norms.chunks_exact_mut(D)) {
            rms(x, w, n);
        }
        if norms.iter().any(|v| !v.is_finite()) {
            return Err(err("non-finite activation"));
        }
        let ids: Vec<u32> = if let Some(index) = &self.readout {
            norms.chunks_exact(D).map(|x|index.best_native(g,out,x)).collect()
        } else {
            let mut scores = vec![0.; V * count];
            native::multiply_tile8(g, out, &norms, &mut scores, count);
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
        let output = &g.tensors["output.weight"];
        let w = g.data(&g.tensors["output_norm.weight"]);
        let mut norms = Vec::with_capacity(D * selected.len());
        for &i in selected {
            let s = &mut sessions[i];
            rms(&s.x, w, &mut s.norm);
            if s.norm.iter().any(|v| !v.is_finite()) {
                return Err(err("non-finite activation"));
            };
            norms.extend_from_slice(&s.norm);
        }
        let ids: Vec<u32> = if let Some(index) = &self.readout {
            norms.chunks_exact(D).map(|x|index.best_native(g,output,x)).collect()
        } else {
            let mut scores = vec![0.; V * selected.len()];
            native::multiply_tile8(g, output, &norms, &mut scores, selected.len());
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
        let get = |n: &str| &g.tensors[n];
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
        let mut x = Vec::with_capacity(b * D);
        for &(_, t) in items {
            x.extend(native::row(g, get("token_embd.weight"), t as usize)?);
        }
        let mut norm = vec![0.; b * D];
        let mut qkv = vec![0.; b * (Q + 2 * H)];
        let mut attn = vec![0.; b * Q];
        let mut proj = vec![0.; b * D];
        let mut ff = vec![0.; b * 2 * FF];
        let mut gated = vec![0.; b * FF];
        for i in 0..L {
            let t = |n: &str| -> &Tensor { get(&format!("blk.{i}.{n}")) };
            let w = |n: &str| g.data(t(n));
            let full = i % 8 == 7;
            for (src, dst) in x.chunks_exact(D).zip(norm.chunks_exact_mut(D)) {
                rms(src, w("attn_norm.weight"), dst);
            }
            self.multiply(t("attn_qkv.weight"), &norm, &mut qkv, b);
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
                for q in q.chunks_exact_mut(H) {
                    rms_inplace(q, w("attn_q_norm.weight"));
                    rope(q, table);
                }
                rms_inplace(k, w("attn_k_norm.weight"));
                rope(k, table);
                let cache = &mut session.cache[i];
                cache.append(pos, k, v);
                attention(q, cache, pos, out, &mut session.scores);
            }
            self.multiply(t("attn_output.weight"), &attn, &mut proj, b);
            for row in proj.chunks_exact_mut(D) {
                rms_inplace(row, w("post_attention_norm"));
            }
            for (x, p) in x.iter_mut().zip(&proj) {
                *x += p;
            }
            for (src, dst) in x.chunks_exact(D).zip(norm.chunks_exact_mut(D)) {
                rms(src, w("ffn_norm.weight"), dst);
            }
            self.multiply(t("ffn_up.weight"), &norm, &mut ff, b);
            for (src, dst) in ff.chunks_exact(2 * FF).zip(gated.chunks_exact_mut(FF)) {
                swiglu(src, dst);
            }
            self.multiply(t("ffn_down.weight"), &gated, &mut proj, b);
            for row in proj.chunks_exact_mut(D) {
                rms_inplace(row, w("post_ffw_norm"));
            }
            for (x, p) in x.iter_mut().zip(&proj) {
                *x += p;
            }
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
fn rope_coeff(pos: usize, table: &[(f32, f32)]) -> [(f32, f32, f32); H / 2] {
    std::array::from_fn(|i| {
        let (freq, scale) = table[i];
        let (s, c) = (pos as f32 * freq).sin_cos();
        (s, c, scale)
    })
}
fn rope(x: &mut [f32], coeff: &[(f32, f32, f32); H / 2]) {
    for (i, &(s, c, scale)) in coeff.iter().enumerate() {
        let (a, b) = (x[i], x[i + H / 2]);
        x[i] = (a * c - b * s) * scale;
        x[i + H / 2] = (b * c + a * s) * scale;
    }
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn dot128_avx(a: &[f32], b: &[f32]) -> f32 {
    use std::arch::x86_64::*;
    unsafe {
        let mut v0 = _mm256_setzero_ps();
        let mut v1 = v0;
        let mut v2 = v0;
        let mut v3 = v0;
        for i in (0..128).step_by(32) {
            v0 = _mm256_add_ps(
                v0,
                _mm256_mul_ps(
                    _mm256_loadu_ps(a.as_ptr().add(i)),
                    _mm256_loadu_ps(b.as_ptr().add(i)),
                ),
            );
            v1 = _mm256_add_ps(
                v1,
                _mm256_mul_ps(
                    _mm256_loadu_ps(a.as_ptr().add(i + 8)),
                    _mm256_loadu_ps(b.as_ptr().add(i + 8)),
                ),
            );
            v2 = _mm256_add_ps(
                v2,
                _mm256_mul_ps(
                    _mm256_loadu_ps(a.as_ptr().add(i + 16)),
                    _mm256_loadu_ps(b.as_ptr().add(i + 16)),
                ),
            );
            v3 = _mm256_add_ps(
                v3,
                _mm256_mul_ps(
                    _mm256_loadu_ps(a.as_ptr().add(i + 24)),
                    _mm256_loadu_ps(b.as_ptr().add(i + 24)),
                ),
            );
        }
        let v = _mm256_add_ps(_mm256_add_ps(v0, v1), _mm256_add_ps(v2, v3));
        let mut tmp = [0.; 8];
        _mm256_storeu_ps(tmp.as_mut_ptr(), v);
        tmp.iter().sum()
    }
}
#[inline]
fn dot128(a: &[f32], b: &[f32]) -> f32 {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") {
        return unsafe { dot128_avx(a, b) };
    }
    a.iter().zip(b).map(|(a, b)| a * b).sum()
}
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn swiglu_avx(src: &[f32], dst: &mut [f32]) {
    use std::arch::x86_64::*;
    unsafe {
        let n = dst.len();
        let one = _mm256_set1_ps(1.);
        let log2e = _mm256_set1_ps(std::f32::consts::LOG2_E);
        let upper = _mm256_set1_ps(80.);
        let lower = _mm256_set1_ps(-80.);
        let ln2_hi = _mm256_set1_ps(0.693359375);
        let ln2_lo = _mm256_set1_ps(-0.00021219444);
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
            let r = _mm256_sub_ps(
                _mm256_sub_ps(x, _mm256_mul_ps(power, ln2_hi)),
                _mm256_mul_ps(power, ln2_lo),
            );
            // On |r| <= ln2/2, degree-seven Taylor is within float32 rounding scale.
            let mut p = _mm256_set1_ps(1. / 5040.);
            for c in [1. / 720., 1. / 120., 1. / 24., 1. / 6., 0.5, 1., 1.] {
                p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(c));
            }
            let e = _mm256_castsi256_ps(_mm256_slli_epi32::<23>(_mm256_add_epi32(
                _mm256_cvttps_epi32(power),
                _mm256_set1_epi32(127),
            )));
            let ex = _mm256_mul_ps(p, e);
            let result = _mm256_mul_ps(_mm256_div_ps(a, _mm256_add_ps(one, ex)), u);
            _mm256_storeu_ps(dst.as_mut_ptr().add(i), result);
        }
    }
}
fn swiglu(src: &[f32], dst: &mut [f32]) {
    #[cfg(target_arch = "x86_64")]
    if std::is_x86_feature_detected!("avx2") && dst.len() % 8 == 0 {
        unsafe { swiglu_avx(src, dst) };
        return;
    }
    let n = dst.len();
    for i in 0..n {
        let a = src[i];
        dst[i] = a / (1. + (-a).exp()) * src[n + i];
    }
}
fn rms(x: &[f32], w: &[u8], o: &mut [f32]) {
    let inv = (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32 + 1e-6)
        .sqrt()
        .recip();
    for ((o, x), w) in o.iter_mut().zip(x).zip(w.chunks_exact(4)) {
        *o = *x * inv * f32::from_le_bytes(w.try_into().unwrap());
    }
}
fn rms_inplace(x: &mut [f32], w: &[u8]) {
    let inv = (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32 + 1e-6)
        .sqrt()
        .recip();
    for (x, w) in x.iter_mut().zip(w.chunks_exact(4)) {
        *x *= inv * f32::from_le_bytes(w.try_into().unwrap());
    }
}
fn attention(q: &[f32], cache: &LayerCache, pos: usize, attn: &mut [f32], scratch: &mut [f32]) {
    let len = (pos + 1).min(cache.capacity);
    let scale = 1. / (H as f32).sqrt();
    for (head, out) in q.chunks_exact(H).zip(attn.chunks_exact_mut(H)) {
        let scores = &mut scratch[..len];
        let mut max = f32::NEG_INFINITY;
        for (idx, s) in scores.iter_mut().enumerate() {
            let offset = ((pos + 1 - len + idx) % cache.capacity) * H;
            *s = dot128(head, &cache.k[offset..offset + H]) * scale;
            max = max.max(*s);
        }
        let mut sum = 0.;
        for s in scores.iter_mut() {
            *s = (*s - max).exp();
            sum += *s;
        }
        out.fill(0.);
        for (idx, s) in scores.iter().enumerate() {
            let offset = ((pos + 1 - len + idx) % cache.capacity) * H;
            let p = *s / sum;
            for (o, v) in out.iter_mut().zip(&cache.v[offset..offset + H]) {
                *o += p * v;
            }
        }
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
        self.step_internal(token, readout, false)
    }
    fn step_internal(&mut self, token: u32, readout: bool, fast: bool) -> Result<Option<u32>> {
        if self.pos >= self.engine.context || token as usize >= V {
            return Err(err("context or token limit"));
        }
        let e = self.engine;
        e.pool.install(|| self.forward(token, readout, fast))
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
        let e = self.engine;
        e.pool.install(|| {
            for chunk in tokens.chunks(32) {
                self.forward_batch(chunk)?;
            }
            self.readout(fast)
        })
    }
    fn readout(&mut self, fast: bool) -> Result<u32> {
        let g = &self.engine.g;
        rms(
            &self.x,
            g.data(&g.tensors["output_norm.weight"]),
            &mut self.norm,
        );
        if self.norm.iter().any(|v| !v.is_finite()) {
            return Err(err("non-finite output activation"));
        }
        if fast {
            if let Some(index) = &self.engine.readout {
                let id = index.best_native(g,&g.tensors["output.weight"], &self.norm);
                if (id as usize) >= V {
                    return Err(err("invalid output token"));
                }
                return Ok(id);
            }
        }
        native::multiply_tile8(g, &g.tensors["output.weight"], &self.norm, &mut self.logits,1);
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
        let g = &self.engine.g;
        let get = |n: &str| -> &Tensor { &g.tensors[n] };
        self.x
            .copy_from_slice(&native::row(g, get("token_embd.weight"), token as usize)?);
        let coeff = [
            rope_coeff(self.pos, &self.rope[0]),
            rope_coeff(self.pos, &self.rope[1]),
        ];
        for i in 0..L {
            let t = |n: &str| -> &Tensor { get(&format!("blk.{i}.{n}")) };
            let w = |n: &str| g.data(t(n));
            let full = i % 8 == 7;
            rms(&self.x, w("attn_norm.weight"), &mut self.norm);
            self.engine
                .multiply(t("attn_qkv.weight"), &self.norm, &mut self.qkv, 1);
            let (q, kv) = self.qkv.split_at_mut(Q);
            let (k, v) = kv.split_at_mut(H);
            let table = &coeff[usize::from(full)];
            for q in q.chunks_exact_mut(H) {
                rms_inplace(q, w("attn_q_norm.weight"));
                rope(q, table);
            }
            rms_inplace(k, w("attn_k_norm.weight"));
            rope(k, table);
            let cache = &mut self.cache[i];
            cache.append(self.pos, k, v);
            attention(q, cache, self.pos, &mut self.attn, &mut self.scores);
            self.engine
                .multiply(t("attn_output.weight"), &self.attn, &mut self.proj, 1);
            rms_inplace(&mut self.proj, w("post_attention_norm"));
            for (x, p) in self.x.iter_mut().zip(&self.proj) {
                *x += p;
            }
            rms(&self.x, w("ffn_norm.weight"), &mut self.norm);
            self.engine
                .multiply(t("ffn_up.weight"), &self.norm, &mut self.ff, 1);
            swiglu(&self.ff, &mut self.gated);
            self.engine
                .multiply(t("ffn_down.weight"), &self.gated, &mut self.proj, 1);
            rms_inplace(&mut self.proj, w("post_ffw_norm"));
            for (x, p) in self.x.iter_mut().zip(&self.proj) {
                *x += p;
            }
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
        assert_eq!(c.k[0], 2.);
        assert_eq!(c.k[H], 1.);
    }
}
