use plamo3_translate_rs::{Engine,Result};
fn main()->Result<()> {
    let args:Vec<_>=std::env::args_os().collect();
    if args.len()!=3 { return Err("Usage: convert_x86 <BF16 model directory> <output.p4x86>".into()); }
    Engine::convert_x86_model(&args[1],&args[2])
}
