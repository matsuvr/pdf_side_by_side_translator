use anyhow::{Result, ensure};
use half::{bf16, f16};
use rayon::prelude::*;
use safetensors::{
    Dtype, SafeTensors,
    tensor::{TensorView, serialize_to_file},
};
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, path::Path};

fn pinned(path: &Path, expected: &str) -> Result<Vec<u8>> {
    let bytes = std::fs::read(path)?;
    ensure!(
        format!("{:x}", Sha256::digest(&bytes)) == expected,
        "source digest mismatch for {}",
        path.display()
    );
    Ok(bytes)
}

fn group(values: &[bf16]) -> (f16, [u32; 4]) {
    let x: [f32; 32] = std::array::from_fn(|i| values[i].to_f32());
    let extreme = x
        .iter()
        .copied()
        .max_by(|a, b| a.abs().total_cmp(&b.abs()))
        .unwrap();
    if extreme == 0.0 {
        return (f16::ZERO, [0x88888888; 4]);
    }
    // Assign the largest magnitude to -8. Fit the scale to all 32
    // coefficients, then round with the scale that will actually be stored.
    let mut scale = -extreme / 8.;
    for _ in 0..3 {
        let inv = scale.recip();
        let (mut xq, mut qq) = (0., 0.);
        for &v in &x {
            let q = (v * inv).round_ties_even().clamp(-8., 7.);
            xq += v * q;
            qq += q * q;
        }
        scale = xq / qq;
    }
    let scale = f16::from_f32(scale);
    let inv = scale.to_f32().recip();
    let packed = std::array::from_fn(|word| {
        x[word * 8..word * 8 + 8]
            .iter()
            .enumerate()
            .fold(0, |bits, (i, &v)| {
                bits | ((((v * inv).round_ties_even().clamp(-8., 7.) as i32 + 8) as u32) << (4 * i))
            })
    });
    (scale, packed)
}

pub fn convert(dir: &Path, dest: &Path) -> Result<()> {
    let raw = pinned(
        &dir.join("model.safetensors"),
        "7c9f0066d166c075bdbe83ada3d444e08bd7a6ee3f345f47a1f3c442aa6898c5",
    )?;
    let tokenizer = pinned(
        &dir.join("tokenizer.jsonl"),
        "bdce9e430e65931b28c56de2054f52b4cf8ed59b322db125e016d724d06dbb0e",
    )?;
    let source = SafeTensors::deserialize(&raw)?;
    let mut tensors = BTreeMap::new();
    tensors.insert(
        "tokenizer.jsonl".to_owned(),
        (Dtype::U8, vec![tokenizer.len()], tokenizer),
    );
    for (name, tensor) in source.iter() {
        ensure!(tensor.dtype() == Dtype::BF16);
        let values: &[bf16] = bytemuck::try_cast_slice(tensor.data())
            .map_err(|_| anyhow::anyhow!("unaligned source"))?;
        match tensor.shape() {
            [rows, cols] => {
                let (rows, cols) = (*rows, *cols);
                ensure!(cols % 32 == 0);
                let mut scales = vec![f16::ZERO; rows * cols / 32];
                let mut packs = vec![[0u32; 4]; scales.len()];
                values
                    .par_chunks_exact(32)
                    .zip(scales.par_iter_mut())
                    .zip(packs.par_iter_mut())
                    .for_each(|((v, s), p)| (*s, *p) = group(v));
                tensors.insert(
                    format!("{name}.q4"),
                    (
                        Dtype::U32,
                        vec![rows, cols / 8],
                        bytemuck::cast_slice(&packs).to_vec(),
                    ),
                );
                tensors.insert(
                    format!("{name}.scale"),
                    (
                        Dtype::F16,
                        vec![rows, cols / 32],
                        bytemuck::cast_slice(&scales).to_vec(),
                    ),
                );
            }
            [cols] => {
                let offset = if name.ends_with("post_mixer_norm.weight") {
                    0.2
                } else if name.ends_with("post_mlp_norm.weight") {
                    1. / 5f32.powf(1.5)
                } else {
                    1.
                };
                let v: Vec<f32> = values.iter().map(|x| x.to_f32() + offset).collect();
                tensors.insert(
                    name.to_owned(),
                    (Dtype::F32, vec![*cols], bytemuck::cast_slice(&v).to_vec()),
                );
            }
            shape => anyhow::bail!("unexpected shape {shape:?}"),
        }
    }
    let views = tensors
        .iter()
        .map(|(name, (ty, shape, data))| Ok((name, TensorView::new(*ty, shape.clone(), data)?)))
        .collect::<Result<Vec<_>>>()?;
    let dir = dest
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let temp = tempfile::NamedTempFile::new_in(dir)?;
    serialize_to_file(views, None, temp.path())?;
    temp.persist_noclobber(dest)?;
    Ok(())
}
