use anyhow::{Context, Result, bail, ensure};
use half::f16;
use rayon::prelude::*;
use safetensors::{
    Dtype,
    tensor::{TensorView, serialize_to_file},
};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashMap},
    path::Path,
};

const EXPECTED_GGUF_SHA256: &str =
    "70891ec70dc7ab4205745a38307867c2881b99239f20327927633abfe4bc9ec6";
const EXPECTED_TOKENIZER_SHA256: &str =
    "bdce9e430e65931b28c56de2054f52b4cf8ed59b322db125e016d724d06dbb0e";

const GGML_TYPE_F32: u32 = 0;
const GGML_TYPE_Q4_K: u32 = 12;
const GGML_TYPE_Q6_K: u32 = 14;

fn pinned(path: &Path, expected: &str) -> Result<Vec<u8>> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    ensure!(
        format!("{:x}", Sha256::digest(&bytes)) == expected,
        "source digest mismatch for {}",
        path.display()
    );
    Ok(bytes)
}

struct Cursor<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn read_bytes(&mut self, n: usize) -> Result<&'a [u8]> {
        ensure!(self.pos + n <= self.data.len(), "unexpected EOF in GGUF");
        let s = &self.data[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.read_bytes(4)?.try_into()?))
    }
    fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.read_bytes(8)?.try_into()?))
    }
    fn str(&mut self) -> Result<String> {
        let len = self.u64()? as usize;
        Ok(String::from_utf8(self.read_bytes(len)?.to_vec())?)
    }
    fn skip_val(&mut self, vt: u32) -> Result<()> {
        match vt {
            0 | 1 | 7 => {
                self.read_bytes(1)?;
            }
            2 | 3 => {
                self.read_bytes(2)?;
            }
            4 | 5 | 6 => {
                self.read_bytes(4)?;
            }
            10 | 11 | 12 => {
                self.read_bytes(8)?;
            }
            8 => {
                let _ = self.str()?;
            }
            9 => {
                let et = self.u32()?;
                let cnt = self.u64()?;
                for _ in 0..cnt {
                    self.skip_val(et)?;
                }
            }
            _ => bail!("unknown GGUF value type {vt}"),
        }
        Ok(())
    }
}

struct GgufTensor {
    gtype: u32,
    dims: Vec<usize>,
    offset: usize,
}

fn parse_gguf(data: &[u8]) -> Result<(usize, HashMap<String, GgufTensor>)> {
    let mut cur = Cursor { data, pos: 0 };
    ensure!(cur.read_bytes(4)? == b"GGUF", "not a GGUF file");
    let ver = cur.u32()?;
    ensure!(ver == 3, "unsupported GGUF version {ver}");
    let n_tensors = cur.u64()? as usize;
    let n_kv = cur.u64()? as usize;
    let mut alignment = 32usize;
    for _ in 0..n_kv {
        let key = cur.str()?;
        let vt = cur.u32()?;
        if key == "general.alignment" && vt == 4 {
            alignment = cur.u32()? as usize;
        } else {
            cur.skip_val(vt)?;
        }
    }
    let mut map = HashMap::with_capacity(n_tensors);
    for _ in 0..n_tensors {
        let name = cur.str()?;
        let nd = cur.u32()? as usize;
        let mut dims = Vec::with_capacity(nd);
        for _ in 0..nd {
            dims.push(cur.u64()? as usize);
        }
        let gtype = cur.u32()?;
        let offset = cur.u64()? as usize;
        map.insert(
            name,
            GgufTensor {
                gtype,
                dims,
                offset,
            },
        );
    }
    let data_start = cur.pos.div_ceil(alignment) * alignment;
    Ok((data_start, map))
}

/// Repack 2 GGUF Q4_K blocks (2 x 144 B = 512 weights) into 1 Metal Q4K_M superblock (288 B):
///   [hdr0 (16 B) | hdr1 (16 B) | linear packed 4-bit nibbles (256 B)]
/// Lossless: exact same scales, mins, and 4-bit codes as GGUF Q4_K.
fn repack_q4k_pair(blk0: &[u8], blk1: &[u8], dst: &mut [u8]) {
    dst[0..16].copy_from_slice(&blk0[0..16]);
    dst[16..32].copy_from_slice(&blk1[0..16]);
    let mut q_lin = [0u8; 512];
    for (b, blk) in [blk0, blk1].into_iter().enumerate() {
        let qs = &blk[16..144];
        let base = b * 256;
        for c in 0..4 {
            for l in 0..32 {
                let byte = qs[32 * c + l];
                q_lin[base + 64 * c + l] = byte & 0x0F;
                q_lin[base + 64 * c + 32 + l] = byte >> 4;
            }
        }
    }
    for i in 0..256 {
        dst[32 + i] = q_lin[2 * i] | (q_lin[2 * i + 1] << 4);
    }
}

fn repack_q4k_matrix(src: &[u8], rows: usize, cols: usize, interleave_gate_up: bool) -> Vec<u8> {
    assert_eq!(cols % 512, 0);
    let src_row_bytes = (cols / 256) * 144;
    let dst_row_bytes = (cols / 512) * 288;
    let sb_per_row = cols / 512;
    let half_rows = rows / 2;
    let mut out = vec![0u8; rows * dst_row_bytes];
    out.par_chunks_exact_mut(dst_row_bytes)
        .enumerate()
        .for_each(|(dst_r, dst_row)| {
            let src_r = if interleave_gate_up {
                if dst_r % 2 == 0 {
                    dst_r / 2
                } else {
                    half_rows + dst_r / 2
                }
            } else {
                dst_r
            };
            let srow = &src[src_r * src_row_bytes..(src_r + 1) * src_row_bytes];
            for sb in 0..sb_per_row {
                let b0 = &srow[(sb * 2) * 144..(sb * 2 + 1) * 144];
                let b1 = &srow[(sb * 2 + 1) * 144..(sb * 2 + 2) * 144];
                repack_q4k_pair(b0, b1, &mut dst_row[sb * 288..(sb + 1) * 288]);
            }
        });
    out
}

fn unpack_q6k_block(blk: &[u8]) -> (f16, [i8; 16], [u8; 256]) {
    let ql = &blk[0..128];
    let qh = &blk[128..192];
    let mut scales = [0i8; 16];
    for i in 0..16 {
        scales[i] = blk[192 + i] as i8;
    }
    let d = f16::from_le_bytes([blk[208], blk[209]]);
    let mut q6 = [0u8; 256];
    for h in 0..2 {
        let qlp = &ql[64 * h..64 * h + 64];
        let qhp = &qh[32 * h..32 * h + 32];
        for l in 0..32 {
            q6[128 * h + l] = (qlp[l] & 0x0F) | ((qhp[l] & 3) << 4);
            q6[128 * h + l + 32] = (qlp[l + 32] & 0x0F) | (((qhp[l] >> 2) & 3) << 4);
            q6[128 * h + l + 64] = (qlp[l] >> 4) | (((qhp[l] >> 4) & 3) << 4);
            q6[128 * h + l + 96] = (qlp[l + 32] >> 4) | (((qhp[l] >> 6) & 3) << 4);
        }
    }
    (d, scales, q6)
}

/// Repack 2 GGUF Q6_K blocks (2 x 210 B = 512 weights) into 1 Metal Q6K_M superblock (432 B):
///   [d0, d1 (4 B) | pad (12 B) | scales[32] (32 B) | linear ql[256] (256 B) | linear qh[128] (128 B)]
/// Lossless: exact same d, int8 scales, and 6-bit codes as GGUF Q6_K.
fn repack_q6k_pair(blk0: &[u8], blk1: &[u8], dst: &mut [u8]) {
    let (d0, sc0, q0) = unpack_q6k_block(blk0);
    let (d1, sc1, q1) = unpack_q6k_block(blk1);
    dst[0..2].copy_from_slice(&d0.to_le_bytes());
    dst[2..4].copy_from_slice(&d1.to_le_bytes());
    dst[4..16].fill(0);
    dst[16..32].copy_from_slice(bytemuck::cast_slice(&sc0));
    dst[32..48].copy_from_slice(bytemuck::cast_slice(&sc1));
    let mut q_all = [0u8; 512];
    q_all[0..256].copy_from_slice(&q0);
    q_all[256..512].copy_from_slice(&q1);
    for i in 0..256 {
        dst[48 + i] = (q_all[2 * i] & 0x0F) | ((q_all[2 * i + 1] & 0x0F) << 4);
    }
    for i in 0..128 {
        dst[304 + i] = ((q_all[4 * i] >> 4) & 3)
            | (((q_all[4 * i + 1] >> 4) & 3) << 2)
            | (((q_all[4 * i + 2] >> 4) & 3) << 4)
            | (((q_all[4 * i + 3] >> 4) & 3) << 6);
    }
}

fn repack_q6k_matrix(src: &[u8], rows: usize, cols: usize) -> Vec<u8> {
    assert_eq!(cols % 512, 0);
    let src_row_bytes = (cols / 256) * 210;
    let dst_row_bytes = (cols / 512) * 432;
    let sb_per_row = cols / 512;
    let mut out = vec![0u8; rows * dst_row_bytes];
    out.par_chunks_exact_mut(dst_row_bytes)
        .enumerate()
        .for_each(|(r, dst_row)| {
            let srow = &src[r * src_row_bytes..(r + 1) * src_row_bytes];
            for sb in 0..sb_per_row {
                let b0 = &srow[(sb * 2) * 210..(sb * 2 + 1) * 210];
                let b1 = &srow[(sb * 2 + 1) * 210..(sb * 2 + 2) * 210];
                repack_q6k_pair(b0, b1, &mut dst_row[sb * 432..(sb + 1) * 432]);
            }
        });
    out
}

/// Build 1-bit sign screening matrix for lm_head (107520 x 1536) from exact dequantized Q6_K weights:
/// Each row = 240 bytes (16-byte aligned):
///   [24 x f16 L1-mean scale (48 B) | 48 x u32 sign bits (192 B)]
fn pack_1bit_screen_from_q6k(src: &[u8], rows: usize, cols: usize) -> Vec<u8> {
    assert_eq!(cols, 1536);
    let src_row_bytes = (cols / 256) * 210;
    let row_bytes = 240usize;
    let mut out = vec![0u8; rows * row_bytes];
    out.par_chunks_exact_mut(row_bytes)
        .enumerate()
        .for_each(|(r, dst)| {
            let srow = &src[r * src_row_bytes..(r + 1) * src_row_bytes];
            let mut vals = [0.0f32; 1536];
            for b in 0..6 {
                let (d, sc, q6) = unpack_q6k_block(&srow[b * 210..(b + 1) * 210]);
                let df = d.to_f32();
                for i in 0..256 {
                    vals[b * 256 + i] = df * (sc[i / 16] as f32) * (q6[i] as i32 - 32) as f32;
                }
            }
            let mut scales = [f16::ZERO; 24];
            let mut signs = [0u32; 48];
            for g in 0..24 {
                let mut l1 = 0.0f32;
                let base = g * 64;
                for j in 0..64 {
                    let v = vals[base + j];
                    l1 += v.abs();
                    if v >= 0.0 {
                        signs[g * 2 + (j / 32)] |= 1u32 << (j % 32);
                    }
                }
                scales[g] = f16::from_f32(l1 / 64.0);
            }
            dst[0..48].copy_from_slice(bytemuck::cast_slice(&scales));
            dst[48..240].copy_from_slice(bytemuck::cast_slice(&signs));
        });
    out
}

pub fn convert(gguf_path: &Path, tokenizer_path: &Path, dest: &Path) -> Result<()> {
    let raw = pinned(gguf_path, EXPECTED_GGUF_SHA256)?;
    let tokenizer = pinned(tokenizer_path, EXPECTED_TOKENIZER_SHA256)?;
    let (data_start, gmap) = parse_gguf(&raw)?;

    let mut tensors: BTreeMap<String, (Dtype, Vec<usize>, Vec<u8>)> = BTreeMap::new();
    tensors.insert(
        "tokenizer.jsonl".to_owned(),
        (Dtype::U8, vec![tokenizer.len()], tokenizer),
    );

    let get_f32 = |gname: &str, expected_len: usize| -> Result<Vec<u8>> {
        let t = gmap
            .get(gname)
            .with_context(|| format!("missing GGUF tensor {gname}"))?;
        ensure!(
            t.gtype == GGML_TYPE_F32 && t.dims == [expected_len],
            "unexpected F32 tensor {gname}: type={} dims={:?}",
            t.gtype,
            t.dims
        );
        let start = data_start + t.offset;
        Ok(raw[start..start + expected_len * 4].to_vec())
    };

    let add_matrix = |tensors: &mut BTreeMap<String, (Dtype, Vec<usize>, Vec<u8>)>,
                      gname: &str,
                      out_name: &str,
                      rows: usize,
                      cols: usize,
                      interleave_gate_up: bool|
     -> Result<()> {
        let t = gmap
            .get(gname)
            .with_context(|| format!("missing GGUF tensor {gname}"))?;
        // GGUF dims are [cols, rows] (inner dimension first).
        ensure!(
            t.dims == [cols, rows],
            "unexpected dims for {gname}: got {:?}, want [{cols}, {rows}]",
            t.dims
        );
        let start = data_start + t.offset;
        match t.gtype {
            GGML_TYPE_Q4_K => {
                let bytes = rows * (cols / 256) * 144;
                let repacked =
                    repack_q4k_matrix(&raw[start..start + bytes], rows, cols, interleave_gate_up);
                tensors.insert(
                    format!("{out_name}.q4k"),
                    (Dtype::U8, vec![rows, (cols / 512) * 288], repacked),
                );
            }
            GGML_TYPE_Q6_K => {
                ensure!(!interleave_gate_up, "unexpected Q6_K gate_up");
                let bytes = rows * (cols / 256) * 210;
                let repacked = repack_q6k_matrix(&raw[start..start + bytes], rows, cols);
                tensors.insert(
                    format!("{out_name}.q6k"),
                    (Dtype::U8, vec![rows, (cols / 512) * 432], repacked),
                );
            }
            other => bail!("unsupported GGUF type {other} for {gname}"),
        }
        Ok(())
    };

    // Embeddings and output head
    add_matrix(&mut tensors, "token_embd.weight", "token_embd.weight", 107520, 1536, false)?;
    {
        let t = gmap.get("output.weight").context("missing output.weight")?;
        ensure!(t.gtype == GGML_TYPE_Q6_K && t.dims == [1536, 107520]);
        let start = data_start + t.offset;
        let bytes = 107520 * (1536 / 256) * 210;
        let slice = &raw[start..start + bytes];
        tensors.insert(
            "output.weight.screen1".to_owned(),
            (
                Dtype::U8,
                vec![107520, 240],
                pack_1bit_screen_from_q6k(slice, 107520, 1536),
            ),
        );
        tensors.insert(
            "output.weight.q6k".to_owned(),
            (
                Dtype::U8,
                vec![107520, (1536 / 512) * 432],
                repack_q6k_matrix(slice, 107520, 1536),
            ),
        );
    }
    tensors.insert(
        "output_norm.weight".to_owned(),
        (Dtype::F32, vec![1536], get_f32("output_norm.weight", 1536)?),
    );

    // 16 transformer layers
    for i in 0..16 {
        let p = |s: &str| format!("blk.{i}.{s}");
        add_matrix(&mut tensors, &p("attn_qkv.weight"), &p("attn_qkv.weight"), 2304, 1536, false)?;
        add_matrix(&mut tensors, &p("attn_output.weight"), &p("attn_output.weight"), 1536, 2048, false)?;
        add_matrix(&mut tensors, &p("ffn_up.weight"), &p("ffn_up.weight"), 12288, 1536, true)?;
        add_matrix(&mut tensors, &p("ffn_down.weight"), &p("ffn_down.weight"), 1536, 6144, false)?;
        for (nm, len) in [
            ("attn_norm.weight", 1536),
            ("attn_q_norm.weight", 128),
            ("attn_k_norm.weight", 128),
            ("post_attention_norm", 1536),
            ("ffn_norm.weight", 1536),
            ("post_ffw_norm", 1536),
        ] {
            let full = p(nm);
            tensors.insert(full.clone(), (Dtype::F32, vec![len], get_f32(&full, len)?));
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
