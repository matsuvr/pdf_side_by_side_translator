#[cfg(windows)]
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
#[cfg(windows)]
use std::{fs::File, io::Read};

#[cfg(windows)]
pub const MODEL: &str = "plamo3-translate-0.8b-x86-s4tile-v7.p4x86";
#[cfg(target_os = "macos")]
pub const MODEL: &str = plamo_metal_q4::MODEL_FILENAME;

pub fn bundled_model() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let directory = exe.parent().ok_or("アプリの場所を確認できません。")?;
    #[cfg(target_os = "macos")]
    let directory = directory
        .ancestors()
        .find(|p| p.extension().is_some_and(|ext| ext == "app"))
        .and_then(Path::parent)
        .unwrap_or(directory);
    Ok(directory.join(MODEL))
}

#[cfg(windows)]
pub struct Translator(plamo3_translate_rs::Engine);

#[cfg(windows)]
impl Translator {
    pub fn open(path: &Path) -> Result<Self, String> {
        let file = File::open(path)
            .map_err(|_| format!("アプリと同じフォルダーに {MODEL} を置いてください。"))?;
        let mut bytes = Vec::new();
        const SIZE: usize = 516_613_376;
        if file.metadata().map_err(|e| e.to_string())?.len() != SIZE as u64 {
            return Err("翻訳モデルのサイズが一致しません。".into());
        }
        bytes
            .try_reserve_exact(SIZE + 1)
            .map_err(|_| "モデル用のメモリを確保できません。")?;
        file.take(SIZE as u64 + 1)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        if format!("{:x}", Sha256::digest(&bytes))
            != "aa290f1609fc0ad49811060a8499a773ababd088b9d21dc3e907c287f4fe2478"
        {
            return Err("翻訳モデルが同梱版と一致しません。".into());
        }
        let threads = std::thread::available_parallelism()
            .map_or(1, |n| n.get().saturating_sub(1).clamp(1, 8));
        plamo3_translate_rs::Engine::from_snapshot(bytes, threads)
            .map(Self)
            .map_err(|e| e.to_string())
    }
    pub fn translate(&self, source: &str, cancelled: impl Fn() -> bool) -> Result<String, String> {
        self.0
            .translate_page(source, cancelled)
            .map_err(|e| e.to_string())
    }
}

#[cfg(target_os = "macos")]
pub struct Translator(plamo_metal_q4::Engine);

#[cfg(target_os = "macos")]
impl Translator {
    pub fn open(path: &Path) -> Result<Self, String> {
        plamo_metal_q4::Engine::open(path).map(Self).map_err(|e| {
            if e.downcast_ref::<std::io::Error>()
                .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound)
            {
                format!("アプリと同じフォルダーに {MODEL} を置いてください。")
            } else {
                e.to_string()
            }
        })
    }
    pub fn translate(&self, source: &str, cancelled: impl Fn() -> bool) -> Result<String, String> {
        self.0
            .translate(source, cancelled)
            .map(|r| r.text)
            .map_err(|e| e.to_string())
    }
}
