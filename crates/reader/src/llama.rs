use super::*;
use std::{
    ffi::{c_char, c_void},
    io::Seek,
    os::fd::IntoRawFd,
    ptr::NonNull,
};

#[allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code
)]
mod sys {
    include!(concat!(env!("OUT_DIR"), "/llama.rs"));
}

unsafe extern "C" fn quiet(_: sys::ggml_log_level, _: *const c_char, _: *mut c_void) {}
struct Stream(NonNull<libc::FILE>);
impl Drop for Stream {
    fn drop(&mut self) {
        unsafe {
            libc::fclose(self.0.as_ptr());
        }
    }
}
struct Context(NonNull<sys::llama_context>);
impl Drop for Context {
    fn drop(&mut self) {
        unsafe { sys::llama_free(self.0.as_ptr()) };
    }
}
pub struct Translator {
    model: NonNull<sys::llama_model>,
    stream: Stream,
    gpu: bool,
}
impl Drop for Translator {
    fn drop(&mut self) {
        unsafe {
            sys::llama_model_free(self.model.as_ptr());
        }
    }
}

impl Translator {
    pub fn open(path: &Path) -> Result<Self, String> {
        let mut file = File::open(path)
            .map_err(|_| format!("アプリと同じフォルダーに {MODEL} を置いてください。"))?;
        let mut hash = Sha256::new();
        std::io::copy(&mut file, &mut hash).map_err(|e| e.to_string())?;
        if format!("{:x}", hash.finalize())
            != "70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6"
        {
            return Err("翻訳モデルが同梱版と一致しません。".into());
        }
        file.rewind().map_err(|e| e.to_string())?;
        let fd = file.into_raw_fd();
        let Some(stream) = NonNull::new(unsafe { libc::fdopen(fd, c"rb".as_ptr()) }) else {
            unsafe { libc::close(fd) };
            return Err("モデルを読み取れません。".into());
        };
        let stream = Stream(stream);
        // llama's backend registry is process-wide. Models and contexts belong
        // exclusively to the inference worker and drop before its stream.
        static INIT: std::sync::Once = std::sync::Once::new();
        INIT.call_once(|| unsafe {
            sys::ggml_log_set(Some(quiet), std::ptr::null_mut());
            sys::llama_log_set(Some(quiet), std::ptr::null_mut());
            sys::llama_backend_init();
        });
        for gpu in [true, false] {
            if let Some(model) = Self::load(&stream, gpu) {
                return Ok(Self { model, stream, gpu });
            }
        }
        Err("翻訳モデルを読み込めません。".into())
    }
    fn load(stream: &Stream, gpu: bool) -> Option<NonNull<sys::llama_model>> {
        let mut params = unsafe { sys::llama_model_default_params() };
        let mut no_devices = std::ptr::null_mut();
        if !gpu {
            params.devices = &mut no_devices;
        }
        params.n_gpu_layers = if gpu { -1 } else { 0 };
        params.load_mode = sys::llama_load_mode_LLAMA_LOAD_MODE_NONE;
        unsafe {
            libc::rewind(stream.0.as_ptr());
        }
        NonNull::new(unsafe {
            sys::llama_model_load_from_file_ptr(stream.0.as_ptr().cast(), params)
        })
    }

    fn tokenize(&self, text: &str, bos: bool, special: bool) -> Result<Vec<i32>, String> {
        let vocab = unsafe { sys::llama_model_get_vocab(self.model.as_ptr()) };
        let mut tokens = vec![0; text.len() + 4];
        let n = unsafe {
            sys::llama_tokenize(
                vocab,
                text.as_ptr().cast(),
                text.len() as i32,
                tokens.as_mut_ptr(),
                tokens.len() as i32,
                bos,
                special,
            )
        };
        if n < 0 {
            return Err("原文をトークンに変換できません。".into());
        }
        tokens.truncate(n as usize);
        Ok(tokens)
    }

    pub fn translate(
        &mut self,
        source: &str,
        cancelled: impl Fn() -> bool,
    ) -> Result<String, String> {
        if source.contains("<|plamo:") {
            return Err("モデルの予約文字列が本文に含まれています。".into());
        }
        let mut tokens = self.tokenize(
            "<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>",
            true,
            true,
        )?;
        tokens.extend(self.tokenize(source, false, false)?);
        tokens.extend(self.tokenize("<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja<|plamo:key|>form<|plamo:val|>plain<|plamo:msg|>",false,true)?);
        let capacity =
            unsafe { sys::llama_model_n_ctx_train(self.model.as_ptr()) }.clamp(0, 32768) as usize;
        if tokens.len() >= capacity {
            return Err("ページがモデルの入力上限を超えています。".into());
        }
        let limit = (tokens.len() * 2 + 128)
            .max(512)
            .min(capacity - tokens.len());
        let mut params = unsafe { sys::llama_context_default_params() };
        params.n_ctx = (tokens.len() + limit) as u32;
        params.n_batch = 128;
        params.n_ubatch = 128;
        let create_context = |model: NonNull<sys::llama_model>| {
            NonNull::new(unsafe { sys::llama_init_from_model(model.as_ptr(), params) })
        };
        let mut context = create_context(self.model);
        if context.is_none()
            && self.gpu
            && !cancelled()
            && let Some(cpu) = Self::load(&self.stream, false)
        {
            unsafe { sys::llama_model_free(self.model.as_ptr()) };
            self.model = cpu;
            self.gpu = false;
            context = create_context(cpu);
        }
        let ctx = Context(context.ok_or("推論用のメモリを確保できません。")?);
        let evaluate = |ids: &mut [i32]| {
            if cancelled() {
                return Err("cancelled");
            }
            let batch = unsafe { sys::llama_batch_get_one(ids.as_mut_ptr(), ids.len() as i32) };
            if unsafe { sys::llama_decode(ctx.0.as_ptr(), batch) } == 0 {
                Ok(())
            } else {
                Err("推論処理を続行できません。")
            }
        };
        for chunk in tokens.chunks_mut(128) {
            evaluate(chunk)?;
        }
        let vocab = unsafe { sys::llama_model_get_vocab(self.model.as_ptr()) };
        let n_vocab = unsafe { sys::llama_vocab_n_tokens(vocab) } as usize;
        let mut result = Vec::new();
        let mut valid = 0;
        for _ in 0..limit {
            if cancelled() {
                return Err("cancelled".into());
            }
            let ptr = NonNull::new(unsafe { sys::llama_get_logits_ith(ctx.0.as_ptr(), -1) })
                .ok_or("推論結果を取得できません。")?;
            let scores = unsafe { std::slice::from_raw_parts(ptr.as_ptr(), n_vocab) };
            let mut best = f32::NEG_INFINITY;
            let mut winner = None;
            for (i, &score) in scores.iter().enumerate() {
                if score <= best || !score.is_finite() {
                    continue;
                }
                let token = i as i32;
                if [1, 2, 16].contains(&token) || unsafe { sys::llama_vocab_is_eog(vocab, token) } {
                    if valid == result.len() {
                        best = score;
                        winner = Some((token, Vec::new()));
                    }
                    continue;
                }
                if unsafe { sys::llama_vocab_is_control(vocab, token) } {
                    continue;
                }
                let mut bytes = vec![0u8; 64];
                let piece = |buf: &mut Vec<u8>| unsafe {
                    sys::llama_token_to_piece(
                        vocab,
                        token,
                        buf.as_mut_ptr().cast(),
                        buf.len() as i32,
                        0,
                        false,
                    )
                };
                let mut n = piece(&mut bytes);
                if n < 0 && n >= -65536 {
                    bytes.resize((-n) as usize, 0);
                    n = piece(&mut bytes);
                }
                if n <= 0 {
                    continue;
                }
                bytes.truncate(n as usize);
                if bytes
                    .iter()
                    .any(|&b| b < 32 && !matches!(b, 9 | 10 | 13) || b == 127)
                {
                    continue;
                }
                let mut tail = result[valid..].to_vec();
                tail.extend_from_slice(&bytes);
                if std::str::from_utf8(&tail).is_err_and(|e| e.error_len().is_some()) {
                    continue;
                }
                best = score;
                winner = Some((token, bytes));
            }
            let (token, bytes) = winner.ok_or("訳文の文字を復号できません。")?;
            if bytes.is_empty() {
                return String::from_utf8(result).map_err(|e| e.to_string());
            }
            result.extend(bytes);
            valid +=
                std::str::from_utf8(&result[valid..]).map_or_else(|e| e.valid_up_to(), |s| s.len());
            evaluate(&mut [token])?;
        }
        Err("ページの翻訳が生成上限に達しました。".into())
    }
}
