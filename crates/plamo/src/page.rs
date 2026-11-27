use super::*;

/// Construct from the exact bytes whose digest was verified by the reader.
impl Engine {
    pub fn from_snapshot(bytes: Vec<u8>, threads: usize) -> Result<Self> {
        Self::from_model(model::from_bytes(bytes)?, threads, 32768, false)
    }

    pub fn translate_page(&self, text: &str, cancelled: impl Fn() -> bool) -> Result<String> {
        let request = TranslationRequest {
            text,
            target: "ja",
            form: Some("plain"),
            ..Default::default()
        };
        let ids = self.tokenizer.encode(&request.prompt()?);
        if ids.len() >= self.context {
            return Err(err("ページがモデルの入力上限を超えています。"));
        }
        let limit = (ids.len() * 2 + 128).max(512).min(self.context - ids.len());
        let _guard = self.pool.enter();
        let mut session = self.session();
        let key = self.prefix_key(&request, &ids);
        let reused = self.restore_prefix(&mut session, key.as_ref());
        for chunk in ids[reused..].chunks(PREFILL_CHUNK) {
            if cancelled() {
                return Err(err("cancelled"));
            }
            session.forward_batch(chunk)?;
        }
        if reused == 0 {
            self.remember_prefix(&session, key.as_ref());
        }
        let mut next = session.readout(true)?;
        let mut text = String::new();
        let mut tail = Vec::new();
        for _ in 0..limit {
            if cancelled() {
                return Err(err("cancelled"));
            }
            let candidate = |id| -> Option<Vec<u8>> {
                if [1, 2, 16].contains(&id) {
                    return tail.is_empty().then(Vec::new);
                }
                let piece = self.tokenizer.output_piece(id)?;
                if piece.is_empty()
                    || piece
                        .iter()
                        .any(|&b| b < 32 && !matches!(b, 9 | 10 | 13) || b == 127)
                {
                    return None;
                }
                let mut bytes = tail.clone();
                bytes.extend_from_slice(&piece);
                match std::str::from_utf8(&bytes) {
                    Err(e) if e.error_len().is_some() => None,
                    _ => Some(bytes),
                }
            };
            let bytes = match candidate(next) {
                Some(bytes) => bytes,
                None => {
                    session.readout(false)?;
                    let mut best = f32::NEG_INFINITY;
                    let mut accepted = None;
                    for (id, &score) in session.logits().iter().enumerate() {
                        if score > best
                            && let Some(bytes) = candidate(id as u32)
                        {
                            best = score;
                            next = id as u32;
                            accepted = Some(bytes);
                        }
                    }
                    accepted.ok_or_else(|| err("訳文の文字を復号できません。"))?
                }
            };
            if bytes.is_empty() {
                return Ok(text);
            }
            let valid =
                std::str::from_utf8(&bytes).map_or_else(|e| e.valid_up_to(), |_| bytes.len());
            text.push_str(std::str::from_utf8(&bytes[..valid])?);
            tail = bytes[valid..].to_vec();
            next = session
                .forward(next, true, true)?
                .ok_or_else(|| err("decode failed"))?;
        }
        Err(err("ページの翻訳が生成上限に達しました。"))
    }
}
