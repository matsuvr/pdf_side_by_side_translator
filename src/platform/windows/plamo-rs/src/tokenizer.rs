use crate::{
    Result, err,
    gguf::{Gguf, Value},
};
use std::collections::HashMap;

pub struct Tokenizer {
    tokens: Vec<String>,
    types: Vec<u32>,
    by_token: HashMap<String, (u32, i64)>,
    bytes: [u32; 256],
    max_len: usize,
}
impl Tokenizer {
    pub(crate) fn load(g: &Gguf) -> Result<Self> {
        let tokens = match g.meta.get("tokenizer.ggml.tokens") {
            Some(Value::Strings(t)) => t.clone(),
            _ => return Err(err("missing tokens")),
        };
        let scores = match g.meta.get("tokenizer.ggml.scores") {
            Some(Value::Floats(t)) => t,
            _ => return Err(err("missing scores")),
        };
        let types = match g.meta.get("tokenizer.ggml.token_type") {
            Some(Value::Ints(t)) => t.clone(),
            _ => return Err(err("missing token types")),
        };
        if tokens.len() != 107520 || scores.len() != tokens.len() || types.len() != tokens.len() {
            return Err(err("bad vocabulary"));
        }
        let mut by_token = HashMap::with_capacity(tokens.len());
        let mut bytes = [0; 256];
        let mut max_len = 0;
        for (i, t) in tokens.iter().enumerate() {
            if types[i] == 6 {
                let b = t
                    .strip_prefix("<0x")
                    .and_then(|s| s.strip_suffix('>'))
                    .and_then(|s| u8::from_str_radix(s, 16).ok())
                    .ok_or_else(|| err("invalid byte token"))?;
                bytes[b as usize] = i as u32;
            } else if !t.is_empty() {
                max_len = max_len.max(t.len());
                by_token.insert(
                    t.clone(),
                    (
                        i as u32,
                        (scores[i] as f64 * 10000.).round_ties_even() as i64,
                    ),
                );
            }
        }
        if bytes.contains(&0) {
            return Err(err("incomplete byte fallback"));
        }
        Ok(Self {
            tokens,
            types,
            by_token,
            bytes,
            max_len,
        })
    }
    pub fn encode(&self, text: &str) -> Vec<u32> {
        self.encode_mode(text, true)
    }
    pub(crate) fn encode_mode(&self, text: &str, special: bool) -> Vec<u32> {
        let mut out = Vec::new();
        let mut start = 0;
        // Explicit PFML special markers and repeated runs are hard boundaries.
        let mut iter = text.char_indices().peekable();
        while let Some((i, c)) = iter.next() {
            if c == '\u{ee00}' {
                self.segment(&text[start..i], &mut out);
                start = i + c.len_utf8();
                continue;
            }
            if special && c == '<' && text[i..].starts_with("<|plamo:") {
                if let Some(end) = text[i..].find("|>") {
                    let end = i + end + 2;
                    let token = &text[i..end];
                    if let Some(&(id, _)) = self.by_token.get(token) {
                        self.segment(&text[start..i], &mut out);
                        out.push(id);
                        while iter.peek().is_some_and(|&(j, _)| j < end) {
                            iter.next();
                        }
                        start = end;
                        continue;
                    }
                }
            }
            let mut count = 1;
            let mut end = i + c.len_utf8();
            while iter.peek().is_some_and(|&(_, d)| d == c) {
                let (j, d) = iter.next().unwrap();
                end = j + d.len_utf8();
                count += 1;
            }
            if (c == ' ' && count >= 2) || (c != '\n' && count >= 4) {
                self.segment(&text[start..i], &mut out);
                self.segment(&text[i..end], &mut out);
                start = end;
            }
        }
        self.segment(&text[start..], &mut out);
        out
    }
    fn segment(&self, text: &str, out: &mut Vec<u32>) {
        if text.is_empty() {
            return;
        }
        let mut pos = text.char_indices().map(|(i, _)| i).collect::<Vec<_>>();
        pos.push(text.len());
        let n = pos.len() - 1;
        let mut best = vec![i64::MIN / 4; n + 1];
        best[n] = 0;
        let mut next = vec![(0usize, None); n];
        for i in (0..n).rev() {
            let limit = pos[i].saturating_add(self.max_len);
            let last = pos.partition_point(|p| *p <= limit) - 1;
            for j in (i + 1..=last).rev() {
                if let Some(&(id, score)) = self.by_token.get(&text[pos[i]..pos[j]]) {
                    if matches!(self.types[id as usize], 2 | 3 | 5) || self.tokens[id as usize].starts_with("<|plamo:") { continue; }
                    let s = score + best[j];
                    if s > best[i] {
                        best[i] = s;
                        next[i] = (j, Some(id));
                    }
                }
            }
            let s = best[i + 1] - 10_000_000;
            if s > best[i] {
                best[i] = s;
                next[i] = (i + 1, None);
            }
        }
        let mut i = 0;
        while i < n {
            let (j, t) = next[i];
            if let Some(id) = t {
                out.push(id)
            } else {
                out.extend(
                    text.as_bytes()[pos[i]..pos[j]]
                        .iter()
                        .map(|b| self.bytes[*b as usize]),
                );
            }
            i = j;
        }
    }
    pub(crate) fn output_piece(&self, id: u32) -> Option<Vec<u8>> {
        let i = id as usize;
        let token = self.tokens.get(i)?;
        if matches!(self.types[i], 2 | 3 | 5) || token.starts_with("<|plamo:") { return None; }
        if self.types[i] == 6 {
            Some(vec![u8::from_str_radix(token.get(3..5)?, 16).ok()?])
        } else { Some(token.as_bytes().to_vec()) }
    }
    pub(crate) fn max_piece_bytes(&self) -> usize { self.max_len.max(1) }
    pub fn decode(&self, ids: &[u32]) -> Result<String> {
        let mut b = Vec::new();
        for &id in ids {
            let i = id as usize;
            let t = self
                .tokens
                .get(i)
                .ok_or_else(|| err("token out of range"))?;
            if self.types[i] == 6 {
                b.push(u8::from_str_radix(&t[3..5], 16)?)
            } else {
                b.extend_from_slice(t.as_bytes());
            }
        }
        Ok(String::from_utf8_lossy(&b).into_owned())
    }
}
