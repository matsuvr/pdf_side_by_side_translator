mod engine;
mod metal;

pub use engine::{Engine, Translation};
pub const MODEL_FILENAME: &str = "plamo3-translate-0.8b-metal-q4.safetensors";
pub const MODEL_BYTES: usize = 504729523;
pub const MODEL_SHA256: &str = "c57e25b3c25b5f2ad4bfecb4f70155a276a854f5d1dc375bd26613b515d385f8";
