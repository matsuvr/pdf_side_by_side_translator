//! P4X86 v7 embedding/head quantization and runtime kernels.
use crate::{Result,err,weights::{Weights,Tensor}};
use rayon::prelude::*;
pub const Q4: u32 = 101;
pub const BLOCK: usize = 32;
pub const BYTES: usize = 20;
pub fn bf16(v: &[u8]) -> f32 {
    f32::from_bits((u16::from_le_bytes([v[0],v[1]]) as u32)<<16)
}
pub(crate) fn quantize(raw:&[u8])->[u8;BYTES] {
    let vals:[f32;BLOCK]=std::array::from_fn(|i|bf16(&raw[2*i..]));
    let max=vals.iter().map(|x|x.abs()).fold(0.,f32::max);
    let d=max/7.;
    let mut out=[0u8;BYTES];
    out[..4].copy_from_slice(&d.to_le_bytes());
    for i in 0..16 {
        // signed -> biased; compute in i32 before cast
        let q=|j:usize| if d==0.{8}else{((vals[j]/d).round_ties_even().clamp(-7.,7.) as i32+8) as u8};
        out[4+i]=q(i)|(q(i+16)<<4);
    }
    out
}
pub fn row_bytes(_ty:u32,n:usize)->usize{n/BLOCK*BYTES}
pub fn row(g:&Weights,t:&Tensor,i:usize)->Result<Vec<f32>> {
    let n=t.dims[0];let size=row_bytes(t.ty,n);
    if t.ty==crate::model::TILE {
        if i>=t.dims[1] {return Err(err("row range"));}
        let groups=n/32;
        let a=(i/8)*groups*160;
        let r=i%8;
        let mut out=Vec::with_capacity(n);
        for b in g.data(t)[a..a+groups*160].chunks_exact(160){
            let d=f32::from_le_bytes(b[r*4..r*4+4].try_into()?);
            for j in 0..32 {
                let k=j%16;
                let byte=b[(k/4+1)*32+r*4+k%4];
                let v=if j<16{byte&15}else{byte>>4};
                out.push(d*(v as f32-8.));
            }
        }
        return Ok(out);
    }
    let b=g.data(t).get(i*size..(i+1)*size).ok_or_else(||err("row range"))?;
    let mut v=Vec::with_capacity(n);
    for b in b.chunks_exact(BYTES){
        let d=f32::from_le_bytes(b[..4].try_into()?);
        for i in 0..BLOCK{let code=if i<16{b[4+i]&15}else{b[4+i-16]>>4};v.push(d*(code as f32-8.));}
    }
    Ok(v)
}
#[derive(Clone)]
struct Q8 { d:f32, vals:[i8;32], sum:i32 }
fn q8(x:&[f32])->Q8 {
    #[cfg(target_arch="x86_64")]
    if std::is_x86_feature_detected!("avx2"){
      let (d,vals)=unsafe{quantize8_avx::<32>(x)};
      return Q8{d,vals,sum:vals.iter().map(|&v|v as i32).sum()};
    }
    let d=x.iter().map(|v|v.abs()).fold(0.,f32::max)/127.;
    let vals=std::array::from_fn(|i| if d==0.{0}else{(x[i]/d).round_ties_even().clamp(-127.,127.) as i8});
    Q8{d,vals,sum:vals.iter().map(|&x|x as i32).sum()}
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2")]
pub(crate) unsafe fn quantize8_avx<const N:usize>(x:&[f32])->(f32,[i8;N]){
    use std::arch::x86_64::*;
    assert_eq!(x.len(),N);assert!(N%32==0);
    unsafe{
        let sign=_mm256_set1_ps(-0.);
        let mut max=_mm256_setzero_ps();
        for i in (0..N).step_by(8){max=_mm256_max_ps(max,_mm256_andnot_ps(sign,_mm256_loadu_ps(x.as_ptr().add(i))));}
        let mut tmp=[0f32;8];_mm256_storeu_ps(tmp.as_mut_ptr(),max);
        let d=tmp.into_iter().fold(0.,f32::max)/127.;
        let mut out=[0i8;N];
        if d==0. {return (d,out);}
        // Divide exactly as scalar; use SIMD nearest-even rounding.
        let scale=_mm256_set1_ps(d);
        for i in (0..N).step_by(32){
            let c=|j:usize|_mm256_cvttps_epi32(_mm256_round_ps::<{_MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC}>(_mm256_div_ps(_mm256_loadu_ps(x.as_ptr().add(i+j)),scale)));
            let p0=_mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(c(0),c(8)));
            let p1=_mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi32(c(16),c(24)));
            let p=_mm256_permute4x64_epi64::<0xD8>(_mm256_packs_epi16(p0,p1));
            _mm256_storeu_si256(out.as_mut_ptr().add(i).cast(),p);
        }
        (d,out)
    }
}
pub fn multiply_tile8(g:&Weights,t:&Tensor,x:&[f32],out:&mut[f32],count:usize){
    let n=t.dims[0];let m=t.dims[1];let groups=n/32;
    let q=x.chunks_exact(32).map(q8).collect::<Vec<_>>();
    let data=g.data(t);
    #[cfg(target_arch="x86_64")]
    let avx2=std::is_x86_feature_detected!("avx2");
    #[cfg(not(target_arch="x86_64"))]
    let avx2=false;
    #[cfg(target_arch="x86_64")]
    let avxvnni=avx2 && !cfg!(feature="force-avx2") && std::is_x86_feature_detected!("avxvnni");
    #[cfg(not(target_arch="x86_64"))]
    let avxvnni=false;
    #[cfg(target_arch="x86_64")]
    let vnni=avx2 && !cfg!(feature="force-avx2") && std::is_x86_feature_detected!("avx512vnni") && std::is_x86_feature_detected!("avx512vl");
    #[cfg(not(target_arch="x86_64"))]
    let vnni=false;
    #[cfg(target_arch="x86_64")]
    if vnni && std::is_x86_feature_detected!("avx512bw") && m%16==0 {
        if count==1 {
            out.par_chunks_exact_mut(16).zip(data.par_chunks_exact(groups*320)).for_each(|(dst,tile)|unsafe{tile16_vnni(tile,&q,groups,dst,1)});
        } else {
            let mut tiles=vec![0.;m*count];
            tiles.par_chunks_exact_mut(count*16).zip(data.par_chunks_exact(groups*320)).for_each(|(dst,tile)|unsafe{tile16_vnni(tile,&q,groups,dst,count)});
            out.par_chunks_exact_mut(m).enumerate().for_each(|(j,dst)|for (i,o) in dst.iter_mut().enumerate(){*o=tiles[(i/16*count+j)*16+i%16]});
        }
        return;
    }
    let calc=|tile:&[u8],dst:&mut[f32]| {
        #[cfg(target_arch="x86_64")]
        if vnni {unsafe{if count==1{tile8_vnni_one(tile,&q,dst)}else{tile8_vnni_many(tile,&q,groups,dst,count)}};return;}
        #[cfg(target_arch="x86_64")]
        if avxvnni {unsafe{if count==1{tile8_avxvnni_one(tile,&q,dst)}else{tile8_avxvnni_many(tile,&q,groups,dst,count)}};return;}
        #[cfg(target_arch="x86_64")]
        if avx2 {
            if std::is_x86_feature_detected!("fma") {
                unsafe{if count==1{tile8_fma_one(tile,&q,dst)}else{tile8_fma_many2(tile,&q,groups,dst,count)}};return;
            }
            if count==1{unsafe{tile8_avx2_one(tile,&q,dst)};return;}
            unsafe{tile8_avx2_many(tile,&q,groups,dst,count)};return;
        }
        let _=(avx2,vnni,avxvnni);
        for (g,b) in tile.chunks_exact(160).enumerate(){
            for j in 0..count {
                let v=&q[j*groups+g];
                for r in 0..8 {
                    let d=f32::from_le_bytes(b[r*4..r*4+4].try_into().unwrap());
                    let mut s=-8*v.sum;
                    for k in 0..16 {
                        let p=b[(k/4+1)*32+r*4+k%4];
                        s+=(p&15) as i32*v.vals[k] as i32+(p>>4) as i32*v.vals[k+16] as i32;
                    }
                    dst[j*8+r]+=d*v.d*s as f32;
                }
            }
        }
    };
    if count==1 {
        out.fill(0.);
        out.par_chunks_exact_mut(8).zip(data.par_chunks_exact(groups*160)).for_each(|(dst,tile)|calc(tile,dst));
    } else {
        let mut tiles=vec![0.;m*count];
        tiles.par_chunks_exact_mut(count*8).zip(data.par_chunks_exact(groups*160)).for_each(|(dst,tile)|calc(tile,dst));
        out.par_chunks_exact_mut(m).enumerate().for_each(|(j,dst)|for(i,o)in dst.iter_mut().enumerate(){*o=tiles[(i/8*count+j)*8+i%8]});
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2")]
unsafe fn tile8_avx2_one(tile:&[u8],q:&[Q8],out:&mut[f32]){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut acc=_mm256_setzero_ps();
        for (b,x) in tile.chunks_exact(160).zip(q){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut pair=_mm256_setzero_si256();
            // Each signed 16-bit lane accumulates eight pair products:
            // 8 * 2 * 15 * 127 = 30480, which is exactly representable.
            for k in 0..4 {
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                let lo=_mm256_and_si256(p,mask);
                let hi=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                pair=_mm256_add_epi16(pair,_mm256_maddubs_epi16(lo,a));
                pair=_mm256_add_epi16(pair,_mm256_maddubs_epi16(hi,c));
            }
            let sum=_mm256_sub_epi32(_mm256_madd_epi16(pair,ones),_mm256_set1_epi32(8*x.sum));
            acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(),acc);
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avx512vnni,avx512vl")]
unsafe fn tile8_vnni_one(tile:&[u8],q:&[Q8],out:&mut[f32]){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);
        let mut acc=_mm256_setzero_ps();
        for (b,x) in tile.chunks_exact(160).zip(q){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut sum=_mm256_set1_epi32(-8*x.sum);
            for k in 0..4 {
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                let lo=_mm256_and_si256(p,mask);
                let hi=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                sum=_mm256_dpbusd_epi32(sum,lo,a);
                sum=_mm256_dpbusd_epi32(sum,hi,c);
            }
            acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(),acc);
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avx512vnni,avx512vl")]
unsafe fn tile8_vnni_many(tile:&[u8],q:&[Q8],groups:usize,out:&mut[f32],count:usize){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);
        for (g,b) in tile.chunks_exact(160).enumerate(){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut low=[_mm256_setzero_si256();4];let mut high=low;
            for k in 0..4{
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                low[k]=_mm256_and_si256(p,mask);
                high[k]=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
            }
            for j in 0..count {
                let x=&q[j*groups+g];
                let mut sum=_mm256_set1_epi32(-8*x.sum);
                for k in 0..4{
                    let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                    let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                    sum=_mm256_dpbusd_epi32(sum,low[k],a);
                    sum=_mm256_dpbusd_epi32(sum,high[k],c);
                }
                let acc=_mm256_loadu_ps(out.as_ptr().add(j*8));
                _mm256_storeu_ps(out.as_mut_ptr().add(j*8),_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d)))));
            }
        }
    }
}

#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avxvnni")]
unsafe fn tile8_avxvnni_one(tile:&[u8],q:&[Q8],out:&mut[f32]){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);
        let mut acc=_mm256_setzero_ps();
        for (b,x) in tile.chunks_exact(160).zip(q){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut sum=_mm256_set1_epi32(-8*x.sum);
            for k in 0..4 {
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                let lo=_mm256_and_si256(p,mask);
                let hi=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                sum=_mm256_dpbusd_avx_epi32(sum,lo,a);
                sum=_mm256_dpbusd_avx_epi32(sum,hi,c);
            }
            acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(),acc);
    }
}

#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avxvnni")]
unsafe fn tile8_avxvnni_many(tile:&[u8],q:&[Q8],groups:usize,out:&mut[f32],count:usize){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);
        for (g,b) in tile.chunks_exact(160).enumerate(){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut low=[_mm256_setzero_si256();4];let mut high=low;
            for k in 0..4{
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                low[k]=_mm256_and_si256(p,mask);
                high[k]=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
            }
            for j in 0..count {
                let x=&q[j*groups+g];
                let mut sum=_mm256_set1_epi32(-8*x.sum);
                for k in 0..4{
                    let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                    let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                    sum=_mm256_dpbusd_avx_epi32(sum,low[k],a);
                    sum=_mm256_dpbusd_avx_epi32(sum,high[k],c);
                }
                let acc=_mm256_loadu_ps(out.as_ptr().add(j*8));
                _mm256_storeu_ps(out.as_mut_ptr().add(j*8),_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d)))));
            }
        }
    }
}

// For prefill, process four input rows at a time and keep the four accumulators
// in registers. The 8-row weight tile fits L1/L2 even for down-projection.
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2")]
unsafe fn tile8_avx2_many(tile:&[u8],q:&[Q8],groups:usize,out:&mut[f32],count:usize){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut j=0;
        while j+4<=count{
            let mut acc=[_mm256_setzero_ps();4];
            for (g,b) in tile.chunks_exact(160).enumerate() {
                let dw=_mm256_loadu_ps(b.as_ptr().cast());
                let mut pair=[_mm256_setzero_si256();4];
                for k in 0..4{
                    let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                    let low=_mm256_and_si256(p,mask);
                    let high=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                    for u in 0..4{
                        let x=&q[(j+u)*groups+g];
                        let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                        let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                        pair[u]=_mm256_add_epi16(pair[u],_mm256_maddubs_epi16(low,a));
                        pair[u]=_mm256_add_epi16(pair[u],_mm256_maddubs_epi16(high,c));
                    }
                }
                for u in 0..4{
                    let x=&q[(j+u)*groups+g];
                    let sum=_mm256_sub_epi32(_mm256_madd_epi16(pair[u],ones),_mm256_set1_epi32(8*x.sum));
                    acc[u]=_mm256_add_ps(acc[u],_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
                }
            }
            for u in 0..4{_mm256_storeu_ps(out.as_mut_ptr().add((j+u)*8),acc[u]);}
            j+=4;
        }
        for j in j..count{tile8_avx2_one(tile,&q[j*groups..(j+1)*groups],&mut out[j*8..j*8+8]);}
    }
}

// Two consecutive eight-row tiles are read directly on AVX512-VNNI CPUs.
// No extra storage or alternative file is needed.
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avx512f,avx512bw,avx512vnni")]
unsafe fn tile16_vnni(tile:&[u8],q:&[Q8],groups:usize,out:&mut[f32],count:usize){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm512_set1_epi8(15);
        let (first,second)=tile.split_at(groups*160);
        let mut single=_mm512_setzero_ps();
        for (g,(b,c)) in first.chunks_exact(160).zip(second.chunks_exact(160)).enumerate(){
            let load=|off:usize|_mm512_inserti64x4::<1>(
                _mm512_castsi256_si512(_mm256_loadu_si256(b.as_ptr().add(off).cast())),
                _mm256_loadu_si256(c.as_ptr().add(off).cast()));
            let dw=_mm512_castsi512_ps(load(0));
            let mut low=[_mm512_setzero_si512();4];let mut high=low;
            for k in 0..4{
                let p=load(32+k*32);
                low[k]=_mm512_and_si512(p,mask);
                high[k]=_mm512_and_si512(_mm512_srli_epi16::<4>(p),mask);
            }
            for j in 0..count {
                let x=&q[j*groups+g];
                let mut s=_mm512_set1_epi32(-8*x.sum);
                for k in 0..4{
                    let a=_mm512_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                    let c=_mm512_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                    s=_mm512_dpbusd_epi32(s,low[k],a);
                    s=_mm512_dpbusd_epi32(s,high[k],c);
                }
                let v=_mm512_mul_ps(_mm512_cvtepi32_ps(s),_mm512_mul_ps(dw,_mm512_set1_ps(x.d)));
                if count==1{single=_mm512_add_ps(single,v);}
                else{
                    let acc=_mm512_loadu_ps(out.as_ptr().add(j*16));
                    _mm512_storeu_ps(out.as_mut_ptr().add(j*16),_mm512_add_ps(acc,v));
                }
            }
        }
        if count==1{_mm512_storeu_ps(out.as_mut_ptr().cast(),single);}
    }
}

// Reuse the same eight-row tile directly for each shortlisted vocabulary
// panel. Avoid scalar horizontal reduction of every single output row.
pub(crate) fn best_tile_panels(g:&Weights,t:&Tensor,x:&[f32],panels:&[usize])->u32{
    let groups=t.dims[0]/32;
    let q=x.chunks_exact(32).map(q8).collect::<Vec<_>>();
    let bytes=g.data(t);
    #[cfg(target_arch="x86_64")]
    let (avx2,vnni,avxvnni)=(
        std::is_x86_feature_detected!("avx2"),
        !cfg!(feature="force-avx2")&&std::is_x86_feature_detected!("avx512vnni")&&std::is_x86_feature_detected!("avx512vl"),
        !cfg!(feature="force-avx2")&&std::is_x86_feature_detected!("avxvnni"));
    #[cfg(not(target_arch="x86_64"))]
    let (avx2,vnni,avxvnni)=(false,false,false);
    if !avx2 {
        let mut best=(f32::NEG_INFINITY,u32::MAX);
        for &p in panels{for i in p*8..p*8+8{
            let r=row(g,t,i).unwrap();
            let s=r.iter().zip(x).map(|(a,b)|a*b).sum();
            if s>best.0||(s==best.0&&(i as u32)<best.1){best=(s,i as u32);}
        }}
        return best.1;
    }
    panels.par_iter().map(|&p|{
        let tile=&bytes[p*groups*160..(p+1)*groups*160];
        let mut dst=[0.;8];
        #[cfg(target_arch="x86_64")]
        unsafe{
            if vnni{tile8_vnni_one(tile,&q,&mut dst);}
            else if avxvnni{tile8_avxvnni_one(tile,&q,&mut dst);}
            else if std::is_x86_feature_detected!("fma"){tile8_fma_one(tile,&q,&mut dst);}
            else{tile8_avx2_one(tile,&q,&mut dst);}
        }
        dst.iter().enumerate().map(|(r,&s)|(s,(p*8+r) as u32))
          .reduce(|a,b|if b.0>a.0||(b.0==a.0&&b.1<a.1){b}else{a}).unwrap()
    }).reduce(||(f32::NEG_INFINITY,u32::MAX),|a,b|if b.0>a.0||(b.0==a.0&&b.1<a.1){b}else{a}).1
}

#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,fma")]
unsafe fn tile8_fma_one(tile:&[u8],q:&[Q8],out:&mut[f32]){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut acc=_mm256_setzero_ps();
        for (b,x) in tile.chunks_exact(160).zip(q){
            let dw=_mm256_loadu_ps(b.as_ptr().cast());
            let mut pairlo=_mm256_setzero_si256();let mut pairhi=pairlo;
            // Each signed 16-bit lane accumulates eight pair products:
            // 8 * 2 * 15 * 127 = 30480, which is exactly representable.
            for k in 0..4 {
                let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                let lo=_mm256_and_si256(p,mask);
                let hi=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                pairlo=_mm256_add_epi16(pairlo,_mm256_maddubs_epi16(lo,a));
                pairhi=_mm256_add_epi16(pairhi,_mm256_maddubs_epi16(hi,c));
            }
            let sum=_mm256_sub_epi32(_mm256_madd_epi16(_mm256_add_epi16(pairlo,pairhi),ones),_mm256_set1_epi32(8*x.sum));
            acc=_mm256_fmadd_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d)),acc);
        }
        _mm256_storeu_ps(out.as_mut_ptr().cast(),acc);
    }
}

#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,fma")]
unsafe fn tile8_fma_many2(tile:&[u8],q:&[Q8],groups:usize,out:&mut[f32],count:usize){
    use std::arch::x86_64::*;
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut j=0;
        while j+2<=count{
            let mut acc=[_mm256_setzero_ps();2];
            for (g,b) in tile.chunks_exact(160).enumerate() {
                let dw=_mm256_loadu_ps(b.as_ptr().cast());
                let mut pairlo=[_mm256_setzero_si256();2];let mut pairhi=pairlo;
                for k in 0..4{
                    let p=_mm256_loadu_si256(b.as_ptr().add(32+k*32).cast());
                    let low=_mm256_and_si256(p,mask);
                    let high=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                    for u in 0..2{
                        let x=&q[(j+u)*groups+g];
                        let a=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(k*4).cast::<i32>()));
                        let c=_mm256_set1_epi32(std::ptr::read_unaligned(x.vals.as_ptr().add(16+k*4).cast::<i32>()));
                        pairlo[u]=_mm256_add_epi16(pairlo[u],_mm256_maddubs_epi16(low,a));
                        pairhi[u]=_mm256_add_epi16(pairhi[u],_mm256_maddubs_epi16(high,c));
                    }
                }
                for u in 0..2{
                    let x=&q[(j+u)*groups+g];
                    let sum=_mm256_sub_epi32(_mm256_madd_epi16(_mm256_add_epi16(pairlo[u],pairhi[u]),ones),_mm256_set1_epi32(8*x.sum));
                    acc[u]=_mm256_fmadd_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d)),acc[u]);
                }
            }
            for u in 0..2{_mm256_storeu_ps(out.as_mut_ptr().add((j+u)*8),acc[u]);}
            j+=2;
        }
        for j in j..count{tile8_fma_one(tile,&q[j*groups..(j+1)*groups],&mut out[j*8..j*8+8]);}
    }
}

