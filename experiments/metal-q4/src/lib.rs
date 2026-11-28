mod engine;
mod metal;

pub use engine::{Engine, Translation};
pub const MODEL_FILENAME: &str = "plamo3-translate-0.8b-metal-q4.safetensors";
pub const MODEL_BYTES: usize = 606167971;
pub const MODEL_SHA256: &str = "61a174371a5163e7a86fe90307e1e06e46c194646ebcef88e8ebd2a20e87b3d0";
