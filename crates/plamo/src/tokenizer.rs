use crate::{
    Result, err,
    weights::{Weights, Value},
};
use std::collections::HashMap;

pub struct Tokenizer {
    tokens: Vec<String>,
    types: Vec<u32>,
    by_token: HashMap<String, (u32, i64)>,
    special_ids: Vec<usize>,
    bytes: [u32; 256],
    max_len: usize,
}
impl Tokenizer {
    pub(crate) fn load(g: &Weights) -> Result<Self> {
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
        // These are the five added tokens in PFN's tokenizer_config.json.
        // The remaining control spellings belong to the ordinary vocabulary.
        let mut special_ids = Vec::new();
        for spelling in ["<|plamo:unk|>", "<|plamo:bos|>", "<|plamo:eos|>", "<|plamo:pad|>", "<|plamo:tag|>"] {
            let &(id, _) = by_token.get(spelling).ok_or_else(|| err("missing added token"))?;
            special_ids.push(id as usize);
        }
        Ok(Self {
            tokens,
            types,
            by_token,
            special_ids,
            bytes,
            max_len,
        })
    }
    pub fn encode(&self, text: &str) -> Vec<u32> {
        self.encode_mode(text, true)
    }
    pub(crate) fn encode_mode(&self, text: &str, special: bool) -> Vec<u32> {
        let mut out = Vec::new();
        if special {
            self.partition_special(text, &self.special_ids, &mut out);
        } else {
            self.encode_raw(text, &mut out);
        }
        out
    }
    // The five added spellings cannot overlap. Transformers isolates them
    // before applying the PLaMo pre-tokenizer to the remaining fragments.
    fn partition_special(&self, text: &str, mut candidates: &[usize], out: &mut Vec<u32>) {
        if text.is_empty() { return; }
        while let Some((&id, rest)) = candidates.split_first() {
            let spelling = self.tokens[id].as_str();
            candidates = rest;
            let mut matches = text.match_indices(spelling);
            let Some(first) = matches.next() else { continue };
            let mut start = 0;
            for (at, matched) in std::iter::once(first).chain(matches) {
                self.partition_special(&text[start..at], rest, out);
                out.push(id as u32);
                start = at + matched.len();
            }
            self.partition_special(&text[start..], rest, out);
            return;
        }
        self.encode_raw(text, out);
    }
    fn encode_raw(&self, text: &str, out: &mut Vec<u32>) {
        const PREFIX: &str = "<|plamo:";
        let mut start = 0;
        let mut search = 0;
        while let Some(offset) = text[search..].find(PREFIX) {
            let begin = search + offset;
            let body = begin + PREFIX.len();
            let name_bytes: usize = text[body..].chars().take(64)
                .take_while(|&c| c != '|' && !c.is_whitespace() && !('\u{1c}'..='\u{1f}').contains(&c))
                .map(char::len_utf8).sum();
            let end = body + name_bytes;
            if text[end..].starts_with("|>") {
                self.segment_runs(&text[start..begin], out);
                start = end + 2;
                self.segment_runs(&text[begin..start], out);
                search = start;
            } else {
                search = body;
            }
        }
        self.segment_runs(&text[start..], out);
    }
    fn segment_runs(&self, text: &str, out: &mut Vec<u32>) {
        let mut start = 0;
        let mut iter = text.char_indices().peekable();
        while let Some((i, c)) = iter.next() {
            if c == '\u{ee00}' {
                self.segment(&text[start..i], out);
                start = i + c.len_utf8();
                continue;
            }
            let mut count = 1;
            let mut end = i + c.len_utf8();
            while iter.peek().is_some_and(|&(_, d)| d == c) {
                let (j, d) = iter.next().unwrap();
                end = j + d.len_utf8();
                count += 1;
            }
            if (c == ' ' && count >= 2) || (c != '\n' && count >= 4) {
                self.segment(&text[start..i], out);
                self.segment(&text[i..end], out);
                start = end;
            }
        }
        self.segment(&text[start..], out);
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    #[ignore = "requires OPT_P4X86_TEST_MODEL pointing to the real S4 v7 model"]
    fn multilingual_pieces_round_trip() -> Result<()> {
        let path = PathBuf::from(std::env::var("OPT_P4X86_TEST_MODEL")?);
        let tokenizer = Tokenizer::load(&crate::model::open(&path)?)?;
        let samples = include_str!("../../../tests/fixtures/plamo3-language-samples.tsv");
        let mut languages = 0;
        for line in samples.lines().filter(|line| !line.starts_with('#')) {
            let (tag, text) = line.split_once('\t').expect("language and sample");
            for source in [text.to_owned(), format!("{text}  १२३ e\u{301} 👩\u{200d}💻 𠮷")] {
                let ids = tokenizer.encode_mode(&source, false);
                let bytes: Vec<u8> = ids.iter().flat_map(|&id| {
                    tokenizer.output_piece(id).expect("ordinary text token")
                }).collect();
                assert_eq!(bytes, source.as_bytes(), "{tag}");
                assert_eq!(tokenizer.decode(&ids)?, source, "{tag}");
            }
            languages += 1;
        }
        assert_eq!(languages, 53);
        Ok(())
    }
}
