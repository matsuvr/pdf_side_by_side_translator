use cubecl::prelude::*;
use half::f16;

// Each SIMD group scores one output row. Its 32 lanes fetch 256 consecutive
// 4-bit weights and consume them directly from registers.
#[cube(launch_unchecked)]
pub fn matvec(
    q: &[u32],
    scale: &[f16],
    x: &[Vector<f32, Const<4>>],
    y: &mut [f32],
    #[comptime] width: usize,
    #[comptime] rows: usize,
) {
    let row = (CUBE_POS_X * 4 + PLANE_POS) as usize;
    let lane = UNIT_POS_PLANE as usize;
    let token = CUBE_POS_Y as usize;
    let mut sum = 0.0f32;
    #[unroll]
    for tile in 0..width / 256 {
        let w = tile * 32 + lane;
        let bits = q[row * (width / 8) + w];
        let xl = x[token * (width / 4) + w * 2];
        let xh = x[token * (width / 4) + w * 2 + 1];
        let mut dot = 0.0f32;
        #[unroll]
        for j in 0usize..4usize {
            let h = Vector::<f16, Const<2>>::reinterpret(
                ((bits >> (j * 4) as u32) & 0x000f000fu32) | 0x64006400u32,
            );
            dot += xl.extract(j) * (f32::cast_from(h.extract(0usize)) - 1032.);
            dot += xh.extract(j) * (f32::cast_from(h.extract(1usize)) - 1032.);
        }

        sum += dot * f32::cast_from(scale[row * (width / 32) + w / 4]);
    }
    let total = plane_sum(sum);
    if lane == 0 {
        y[token * rows + row] = total;
    }
}

#[cube(launch)]
pub fn embed(q: &[u32], scale: &[f16], ids: &[u32], out: &mut [f32]) {
    let i = ABSOLUTE_POS;
    let w = ids[i / 1536] as usize * 1536 + i % 1536;
    let bits = (q[w / 8] >> ((w % 8) * 4) as u32) & 15;
    out[i] = (f32::cast_from(bits) - 8.0) * f32::cast_from(scale[w / 32]);
}

// A single SIMD group owns a complete RMS normalization. The residual
// variant fuses the addition after the post-attention and post-MLP norms.
#[cube(launch)]
pub fn norm(
    x: &[f32],
    weight: &[f32],
    residual: &[f32],
    out: &mut [f32],
    offset: u32,
    #[comptime] add: bool,
) {
    let token = CUBE_POS_X as usize;
    let base = (token + offset as usize) * 1536;
    let lane = UNIT_POS as usize;
    let mut sq = 0.0f32;
    for i in 0..48 {
        let v = x[base + i * 32 + lane];
        sq += v * v;
    }
    let inv = f32::recip(f32::sqrt(plane_sum(sq) / 1536.0 + 1e-6));
    for i in 0..48 {
        let col = i * 32 + lane;
        let v = x[base + col] * inv * weight[col];
        out[token * 1536 + col] = if add { v + residual[base + col] } else { v };
    }
}

#[cube(launch)]
pub fn prepare_qkv(
    qkv: &[f32],
    qnorm: &[f32],
    knorm: &[f32],
    rope: &[f32],
    q: &mut [f32],
    keys: &mut [f16],
    values: &mut [f16],
    start: u32,
) {
    let head = CUBE_POS_X as usize;
    let token = CUBE_POS_Y as usize;
    let pos = start as usize + token;
    let lane = UNIT_POS as usize;
    let base = token * 2304usize + head * 128usize;
    let mut sq = 0.0f32;
    #[unroll]
    for j in 0usize..4usize {
        let v = qkv[base + j * 32usize + lane];
        sq += v * v;
    }
    let inv = f32::recip(f32::sqrt(plane_sum(sq) / 128. + 1e-6f32));
    #[unroll]
    for j in 0usize..2usize {
        let i = lane + j * 32usize;
        let angle = f32::cast_from(pos) * rope[i];
        let c = f32::cos(angle) * rope[64usize];
        let s = f32::sin(angle) * rope[64usize];
        let a = qkv[base + i] * inv;
        let b = qkv[base + i + 64usize] * inv;
        if head == 16usize {
            let a = a * knorm[i];
            let b = b * knorm[i + 64usize];
            keys[pos * 128usize + i] = f16::cast_from(a * c - b * s);
            keys[pos * 128usize + i + 64usize] = f16::cast_from(b * c + a * s);
            values[pos * 128usize + i] = f16::cast_from(qkv[base + 128usize + i]);
            values[pos * 128usize + i + 64usize] = f16::cast_from(qkv[base + 192usize + i]);
        } else {
            let a = a * qnorm[i];
            let b = b * qnorm[i + 64usize];
            let dest = (token * 16usize + head) * 128usize + i;
            q[dest] = a * c - b * s;
            q[dest + 64usize] = b * c + a * s;
        }
    }
}

#[cube(launch)]
pub fn attention(
    q: &[f32],
    keys: &[f16],
    values: &[f16],
    out: &mut [f32],
    start: u32,
    #[comptime] full: bool,
) {
    let token = CUBE_POS_Y as usize;
    let pos = token + start as usize;
    let head = CUBE_POS_X as usize;
    let lane = UNIT_POS_PLANE as usize;
    let plane = PLANE_POS as usize;
    let mut partial: Shared<[f32]> = Shared::new_slice(1040usize);
    let base = (token * 16 + head) * 128 + lane;
    let first = if full || pos < 2048 {
        0usize
    } else {
        pos + 1 - 2048
    };
    let mut peak = f32::new(-1.0e30f32);
    let mut total = 0.0f32;
    let mut acc = Array::<f32>::new(4usize);
    let mut query = Array::<f32>::new(4usize);
    #[unroll]
    for j in 0usize..4usize {
        acc[j] = 0.;
        query[j] = q[base + j * 32];
    }
    let end = pos + 1;
    for step in 0usize..(end - first + 7 - plane) / 8 {
        let p = first + plane + step * 8;
        let idx = p * 128 + lane;
        let mut dot = 0.0f32;
        #[unroll]
        for j in 0usize..4usize {
            dot += query[j] * f32::cast_from(keys[idx + j * 32]);
        }
        let score = plane_sum(dot) * 0.08838834765;
        let next = f32::max(peak, score);
        let old = f32::exp(peak - next);
        let w = f32::exp(score - next);
        #[unroll]
        for j in 0usize..4usize {
            acc[j] = acc[j] * old + w * f32::cast_from(values[idx + j * 32]);
        }
        total = total * old + w;
        peak = next;
    }
    #[unroll]
    for j in 0usize..4usize {
        partial[plane * 128 + j * 32 + lane] = acc[j];
    }
    if lane == 0 {
        partial[1024usize + plane] = peak;
        partial[1032usize + plane] = total;
    }
    sync_cube();
    if plane == 0 {
        let mut maximum = f32::new(-1.0e30f32);
        #[unroll]
        for i in 0usize..8usize {
            maximum = f32::max(maximum, partial[1024usize + i]);
        }
        let mut weight_sum = 0.0f32;
        #[unroll]
        for j in 0usize..4usize {
            acc[j] = 0.;
        }
        #[unroll]
        for i in 0usize..8usize {
            let w = f32::exp(partial[1024usize + i] - maximum);
            weight_sum += w * partial[1032usize + i];
            #[unroll]
            for j in 0usize..4usize {
                acc[j] += w * partial[i * 128 + j * 32 + lane];
            }
        }
        #[unroll]
        for j in 0usize..4usize {
            out[base + j * 32] = acc[j] / weight_sum;
        }
    }
}

#[cube(launch)]
pub fn swiglu(ff: &[f32], y: &mut [f32]) {
    let i = ABSOLUTE_POS;
    let base = i / 6144 * 12288 + i % 6144;
    let gate = ff[base];
    y[i] = gate / (1. + f32::exp(-gate)) * ff[base + 6144];
}

// Prefill shares each 64x32 weight tile between 32 prompt tokens. Each
// of eight SIMD groups accumulates four 8x8 FP32 output fragments.
#[cube(launch_unchecked)]
pub fn matmul(
    q: &[u32],
    scale: &[f16],
    x: &[f32],
    y: &mut [f32],
    count: u32,
    #[comptime] width: usize,
    #[comptime] rows: usize,
) {
    let unit = UNIT_POS as usize;
    let plane = PLANE_POS as usize;
    let row = CUBE_POS_X as usize * 64;
    let tok = CUBE_POS_Y as usize * 32;
    let mut a: Shared<[f16]> = Shared::new_slice(1024usize);
    let mut b: Shared<[f16]> = Shared::new_slice(2048usize);
    let acc00 = cmma::Matrix::<f32>::from_value(
        cmma::MatrixIdent::Accumulator,
        8usize,
        8usize,
        8usize,
        cmma::MatrixLayout::Undefined,
        0.0f32,
    );
    let acc01 = cmma::Matrix::<f32>::from_value(
        cmma::MatrixIdent::Accumulator,
        8usize,
        8usize,
        8usize,
        cmma::MatrixLayout::Undefined,
        0.0f32,
    );
    let acc10 = cmma::Matrix::<f32>::from_value(
        cmma::MatrixIdent::Accumulator,
        8usize,
        8usize,
        8usize,
        cmma::MatrixLayout::Undefined,
        0.0f32,
    );
    let acc11 = cmma::Matrix::<f32>::from_value(
        cmma::MatrixIdent::Accumulator,
        8usize,
        8usize,
        8usize,
        cmma::MatrixLayout::Undefined,
        0.0f32,
    );
    for tile in 0..width / 32 {
        #[unroll]
        for j in 0usize..4usize {
            let i = unit + j * 256;
            let t = tok + i / 32;
            a[i] = if t < count as usize {
                f16::cast_from(x[t * width + tile * 32 + i % 32])
            } else {
                f16::from_int(0)
            };
        }
        {
            let r = row + unit / 4;
            let bits = q[r * (width / 8) + tile * 4 + unit % 4];
            let d = scale[r * (width / 32) + tile];
            #[unroll]
            for j in 0usize..8usize {
                let v = f32::cast_from((bits >> (j * 4) as u32) & 15) - 8.;
                b[unit * 8 + j] = f16::cast_from(v) * d;
            }
        }
        sync_cube();
        #[unroll]
        for k in 0usize..4usize {
            let ai = (plane / 4) * 512 + k * 8;
            let bi = (plane % 4) * 512 + k * 8;
            let a0 = cmma::Matrix::<f16>::from_slice(
                cmma::MatrixIdent::A,
                8usize,
                8usize,
                8usize,
                cmma::MatrixLayout::RowMajor,
                &a[ai..1024usize],
                32,
            );
            let a1 = cmma::Matrix::<f16>::from_slice(
                cmma::MatrixIdent::A,
                8usize,
                8usize,
                8usize,
                cmma::MatrixLayout::RowMajor,
                &a[ai + 256..1024usize],
                32,
            );
            let b0 = cmma::Matrix::<f16>::from_slice(
                cmma::MatrixIdent::B,
                8usize,
                8usize,
                8usize,
                cmma::MatrixLayout::ColMajor,
                &b[bi..2048usize],
                32,
            );
            let b1 = cmma::Matrix::<f16>::from_slice(
                cmma::MatrixIdent::B,
                8usize,
                8usize,
                8usize,
                cmma::MatrixLayout::ColMajor,
                &b[bi + 256..2048usize],
                32,
            );
            cmma::execute(&a0, &b0, &acc00, &acc00);
            cmma::execute(&a0, &b1, &acc01, &acc01);
            cmma::execute(&a1, &b0, &acc10, &acc10);
            cmma::execute(&a1, &b1, &acc11, &acc11);
        }
        sync_cube();
    }
    let dest = (tok + (plane / 4) * 16) * rows + row + (plane % 4) * 16;
    let len = (tok + 32) * rows;
    cmma::store(
        &mut y[dest..len],
        &acc00,
        rows as u32,
        cmma::MatrixLayout::RowMajor,
    );
    cmma::store(
        &mut y[dest + 8..len],
        &acc01,
        rows as u32,
        cmma::MatrixLayout::RowMajor,
    );
    cmma::store(
        &mut y[dest + 8 * rows..len],
        &acc10,
        rows as u32,
        cmma::MatrixLayout::RowMajor,
    );
    cmma::store(
        &mut y[dest + 8 * rows + 8..len],
        &acc11,
        rows as u32,
        cmma::MatrixLayout::RowMajor,
    );
}
