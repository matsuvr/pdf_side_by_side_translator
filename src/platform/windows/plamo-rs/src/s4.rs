// S4: 256-weight superblock containing 8 independent 32-weight scales.
// Symmetric weights: 4-bit biased nibbles; fp32 global scale and u8 local
// scales. At runtime scale multiplication and accumulation are integer until
// the entire superblock is complete. 4.375 bits per matrix weight.
use crate::{weights::{Weights,Tensor},native};
use rayon::prelude::*;
const N:usize=256;
const B:usize=140;
pub fn quantize_matrix(raw:&[u8])->Vec<u8>{
    raw.par_chunks_exact(N*2).flat_map_iter(|b|{
        let raw:[f32;N]=std::array::from_fn(|i|native::bf16(&b[i*2..]));
        let mut ds=[0.;8];
        for g in 0..8{
            let a=&raw[g*32..g*32+32];
            let max=a.iter().map(|x|x.abs()).fold(0.,f32::max);
            let mut best=f32::INFINITY;let mut bestd=max/7.;
            for r in [7.,7.5,8.,8.5]{
                let mut d=max/r;
                if d>0.{for _ in 0..2{
                    let (mut xy,mut yy)=(0.,0.);
                    for &x in a{let q=(x/d).round_ties_even().clamp(-7.,7.);xy+=x*q;yy+=q*q;}
                    if yy>0.{d=xy/yy;}
                }}
                let mse=a.iter().map(|&x| {let q=if d==0.{0.}else{(x/d).round_ties_even().clamp(-7.,7.)};(x-d*q).powi(2)}).sum::<f32>();
                if mse<best{best=mse;bestd=d;}
            }
            ds[g]=bestd;
        }
        let d=ds.iter().copied().fold(0.,f32::max)/127.;
        let mut o=[0u8;B];o[..4].copy_from_slice(&d.to_le_bytes());
        for g in 0..8 {
            let s=if d==0.{0}else{(ds[g]/d).round_ties_even().clamp(1.,127.)as u8};
            o[4+g]=s;let scale=d*s as f32;
            for i in 0..16{
                let q=|j:usize|if scale==0.{8}else{((raw[g*32+j]/scale).round_ties_even().clamp(-7.,7.)as i32+8)as u8};
                o[12+16*g+i]=q(i)|(q(i+16)<<4);
            }
        }
        o
    }).collect()
}
struct Act{d:f32,low:[i8;128],high:[i8;128],sums:[i32;8]}
fn act(x:&[f32])->Act{
    #[cfg(target_arch="x86_64")]
    let (d,q)=if std::is_x86_feature_detected!("avx2"){unsafe{native::quantize8_avx::<N>(x)}}else{scalar_act(x)};
    #[cfg(not(target_arch="x86_64"))]
    let (d,q)=scalar_act(x);
    let mut lo=[0;128];let mut hi=[0;128];
    for i in 0..8{lo[i*16..i*16+16].copy_from_slice(&q[i*32..i*32+16]);hi[i*16..i*16+16].copy_from_slice(&q[i*32+16..i*32+32]);}
    let sums=std::array::from_fn(|i|q[i*32..(i+1)*32].iter().map(|&x|x as i32).sum());
    Act{d,low:lo,high:hi,sums}
}
fn scalar_act(x:&[f32])->(f32,[i8;N]){
    let d=x.iter().map(|x|x.abs()).fold(0.,f32::max)/127.;
    (d,std::array::from_fn(|i|if d==0.{0}else{(x[i]/d).round_ties_even().clamp(-127.,127.)as i8}))
}
#[cfg(test)]
fn dot_scalar(row:&[u8],a:&[Act])->f32{
    row.chunks_exact(B).zip(a).map(|(b,x)|{
        let dw=f32::from_le_bytes(b[..4].try_into().unwrap());
        let sum=(0..8).map(|g|{
           let s=(0..16).map(|i|{
               let v=b[12+g*16+i];((v&15)as i32-8)*x.low[g*16+i]as i32+((v>>4)as i32-8)*x.high[g*16+i]as i32
           }).sum::<i32>();
           s*b[4+g]as i32
        }).sum::<i32>();
        sum as f32*dw*x.d
    }).sum()
}
#[cfg(target_arch="x86_64")]
use std::arch::x86_64::*;
// Same S4 quantization in output-major tiles, with no duplicated storage.
const TB:usize=B*8;
pub fn tile8(src:&[u8],n:usize,m:usize)->Vec<u8>{
    let blocks=n/256;let mut dst=vec![0;src.len()];
    assert_eq!(m%8,0);
    dst.par_chunks_exact_mut(blocks*TB).enumerate().for_each(|(tile,out)|{
       for b in 0..blocks{
           let d=&mut out[b*TB..(b+1)*TB];
           for i in 0..8{
               let w=&src[((tile*8+i)*blocks+b)*B..((tile*8+i)*blocks+b+1)*B];
               d[i*4..i*4+4].copy_from_slice(&w[..4]);
               for g in 0..8{d[32+g*8+i]=w[4+g];for k in 0..4{
                   let o=96+(g*4+k)*32+i*4;
                   d[o..o+4].copy_from_slice(&w[12+g*16+k*4..12+g*16+k*4+4]);
               }}
           }
       }
    });
    dst
}
pub fn multiply_tile(g:&Weights,t:&Tensor,x:&[f32],out:&mut[f32],count:usize){
    let n=t.dims[0];let m=t.dims[1];let blocks=n/256;let a=x.chunks_exact(256).map(act).collect::<Vec<_>>();let data=g.data(t);
    let mut temp=vec![0.;out.len()];
    let calc=|d:&mut[f32],tile:&[u8]|{
        #[cfg(target_arch="x86_64")]
        if std::is_x86_feature_detected!("avx2"){
            unsafe{
              if !cfg!(feature="force-avx2")&&std::is_x86_feature_detected!("avx512vnni")&&std::is_x86_feature_detected!("avx512vl"){tile_vnni(tile,&a,blocks,d,count)}
              else if !cfg!(feature="force-avx2")&&std::is_x86_feature_detected!("avxvnni"){tile_avxvnni(tile,&a,blocks,d,count)}
              else if std::is_x86_feature_detected!("fma"){tile_fma(tile,&a,blocks,d,count)}
              else{tile_avx(tile,&a,blocks,d,count)}
            };return;
        }
        for (b,w) in tile.chunks_exact(TB).enumerate(){
            for j in 0..count{
               let x=&a[j*blocks+b];
               for r in 0..8{
                  let scale=f32::from_le_bytes(w[r*4..r*4+4].try_into().unwrap());
                  let mut sum=0i32;
                  for g in 0..8{
                     let mut s=-8*x.sums[g];
                     for k in 0..16{
                         let v=w[96+(g*4+k/4)*32+r*4+k%4];
                         s+=(v&15)as i32*x.low[g*16+k]as i32+(v>>4)as i32*x.high[g*16+k]as i32;
                     }
                     sum+=s*w[32+g*8+r]as i32;
                  }
                  d[j*8+r]+=sum as f32*scale*x.d;
               }
            }
        }
    };
    if count==1{out.fill(0.);out.par_chunks_exact_mut(8).zip(data.par_chunks_exact(blocks*TB)).for_each(|(d,t)|calc(d,t));return;}
    temp.par_chunks_exact_mut(8*count).zip(data.par_chunks_exact(blocks*TB)).for_each(|(d,t)|calc(d,t));
    out.par_chunks_exact_mut(m).enumerate().for_each(|(j,d)|for(i,p)in d.chunks_exact_mut(8).enumerate(){p.copy_from_slice(&temp[(i*count+j)*8..(i*count+j+1)*8])});
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2")]
unsafe fn tile_avx(tile:&[u8],a:&[Act],blocks:usize,out:&mut[f32],count:usize){
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut base=0;
        while base+2<=count{
            let mut acc=[_mm256_setzero_ps();2];
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=[&a[base*blocks+b],&a[(base+1)*blocks+b]];
                let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=[_mm256_setzero_si256();2];
                for g in 0..8{
                    let mut pair=[_mm256_setzero_si256();2];
                    for k in 0..4{
                        let v=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(v,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(v),mask);
                        for j in 0..2{
                            let al=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].low.as_ptr().add(g*16+k*4).cast::<i32>()));
                            let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].high.as_ptr().add(g*16+k*4).cast::<i32>()));
                            pair[j]=_mm256_add_epi16(pair[j],_mm256_maddubs_epi16(l,al));
                            pair[j]=_mm256_add_epi16(pair[j],_mm256_maddubs_epi16(h,ah));
                        }
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    for j in 0..2{
                       let s=_mm256_sub_epi32(_mm256_madd_epi16(pair[j],ones),_mm256_set1_epi32(8*x[j].sums[g]));
                       sum[j]=_mm256_add_epi32(sum[j],_mm256_mullo_epi32(s,scale));
                    }
                }
                for j in 0..2{
                    acc[j]=_mm256_add_ps(acc[j],_mm256_mul_ps(_mm256_cvtepi32_ps(sum[j]),_mm256_mul_ps(dw,_mm256_set1_ps(x[j].d))));
                }
            }
            for j in 0..2{_mm256_storeu_ps(out.as_mut_ptr().add((base+j)*8),acc[j]);}
            base+=2;
        }
        if base<count{
            let mut acc=_mm256_setzero_ps();
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=&a[base*blocks+b];let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=_mm256_setzero_si256();
                for g in 0..8{
                    let mut pair=_mm256_setzero_si256();
                    for k in 0..4{
                        let p=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(p,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                        let al=_mm256_set1_epi32(std::ptr::read_unaligned(x.low.as_ptr().add(g*16+k*4).cast::<i32>()));
                        let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x.high.as_ptr().add(g*16+k*4).cast::<i32>()));
                        pair=_mm256_add_epi16(pair,_mm256_maddubs_epi16(l,al));
                        pair=_mm256_add_epi16(pair,_mm256_maddubs_epi16(h,ah));
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    let dot=_mm256_sub_epi32(_mm256_madd_epi16(pair,ones),_mm256_set1_epi32(8*x.sums[g]));
                    sum=_mm256_add_epi32(sum,_mm256_mullo_epi32(dot,scale));
                }
                acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
            }
            _mm256_storeu_ps(out.as_mut_ptr().add(base*8),acc);
        }
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,fma")]
unsafe fn tile_fma(tile:&[u8],a:&[Act],blocks:usize,out:&mut[f32],count:usize){
    unsafe{
        let mask=_mm256_set1_epi8(15);let ones=_mm256_set1_epi16(1);
        let mut base=0;
        while base+2<=count{
            let mut acc=[_mm256_setzero_ps();2];
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=[&a[base*blocks+b],&a[(base+1)*blocks+b]];
                let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=[_mm256_setzero_si256();2];
                for g in 0..8{
                    let mut pl=[_mm256_setzero_si256();2];let mut ph=pl;
                    for k in 0..4{
                        let v=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(v,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(v),mask);
                        for j in 0..2{
                            let al=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].low.as_ptr().add(g*16+k*4).cast::<i32>()));
                            let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].high.as_ptr().add(g*16+k*4).cast::<i32>()));
                            pl[j]=_mm256_add_epi16(pl[j],_mm256_maddubs_epi16(l,al));
                            ph[j]=_mm256_add_epi16(ph[j],_mm256_maddubs_epi16(h,ah));
                        }
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    for j in 0..2{
                       let s=_mm256_sub_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl[j],ph[j]),ones),_mm256_set1_epi32(8*x[j].sums[g]));
                       sum[j]=_mm256_add_epi32(sum[j],_mm256_mullo_epi32(s,scale));
                    }
                }
                for j in 0..2{
                    acc[j]=_mm256_fmadd_ps(_mm256_cvtepi32_ps(sum[j]),_mm256_mul_ps(dw,_mm256_set1_ps(x[j].d)),acc[j]);
                }
            }
            for j in 0..2{_mm256_storeu_ps(out.as_mut_ptr().add((base+j)*8),acc[j]);}
            base+=2;
        }
        if base<count{
            let mut acc=_mm256_setzero_ps();
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=&a[base*blocks+b];let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=_mm256_setzero_si256();
                for g in 0..8{
                    let mut pl=_mm256_setzero_si256();let mut ph=pl;
                    for k in 0..4{
                        let p=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(p,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                        let al=_mm256_set1_epi32(std::ptr::read_unaligned(x.low.as_ptr().add(g*16+k*4).cast::<i32>()));
                        let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x.high.as_ptr().add(g*16+k*4).cast::<i32>()));
                        pl=_mm256_add_epi16(pl,_mm256_maddubs_epi16(l,al));
                        ph=_mm256_add_epi16(ph,_mm256_maddubs_epi16(h,ah));
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    let dot=_mm256_sub_epi32(_mm256_madd_epi16(_mm256_add_epi16(pl,ph),ones),_mm256_set1_epi32(8*x.sums[g]));
                    sum=_mm256_add_epi32(sum,_mm256_mullo_epi32(dot,scale));
                }
                acc=_mm256_fmadd_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d)),acc);
            }
            _mm256_storeu_ps(out.as_mut_ptr().add(base*8),acc);
        }
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avx512vnni,avx512vl")]
unsafe fn tile_vnni(tile:&[u8],a:&[Act],blocks:usize,out:&mut[f32],count:usize){
    unsafe{
        let mask=_mm256_set1_epi8(15);
        let mut base=0;
        while base+2<=count{
            let mut acc=[_mm256_setzero_ps();2];
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=[&a[base*blocks+b],&a[(base+1)*blocks+b]];
                let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=[_mm256_setzero_si256();2];
                for g in 0..8{
                    let mut pair=[_mm256_setzero_si256();2];
                    for k in 0..4{
                        let v=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(v,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(v),mask);
                        for j in 0..2{
                            let al=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].low.as_ptr().add(g*16+k*4).cast::<i32>()));
                            let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].high.as_ptr().add(g*16+k*4).cast::<i32>()));
                            pair[j]=_mm256_dpbusd_epi32(pair[j],l,al);
                            pair[j]=_mm256_dpbusd_epi32(pair[j],h,ah);
                        }
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    for j in 0..2{
                       let s=_mm256_sub_epi32(pair[j],_mm256_set1_epi32(8*x[j].sums[g]));
                       sum[j]=_mm256_add_epi32(sum[j],_mm256_mullo_epi32(s,scale));
                    }
                }
                for j in 0..2{
                    acc[j]=_mm256_add_ps(acc[j],_mm256_mul_ps(_mm256_cvtepi32_ps(sum[j]),_mm256_mul_ps(dw,_mm256_set1_ps(x[j].d))));
                }
            }
            for j in 0..2{_mm256_storeu_ps(out.as_mut_ptr().add((base+j)*8),acc[j]);}
            base+=2;
        }
        if base<count{
            let mut acc=_mm256_setzero_ps();
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=&a[base*blocks+b];let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=_mm256_setzero_si256();
                for g in 0..8{
                    let mut pair=_mm256_setzero_si256();
                    for k in 0..4{
                        let p=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(p,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                        let al=_mm256_set1_epi32(std::ptr::read_unaligned(x.low.as_ptr().add(g*16+k*4).cast::<i32>()));
                        let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x.high.as_ptr().add(g*16+k*4).cast::<i32>()));
                        pair=_mm256_dpbusd_epi32(pair,l,al);
                        pair=_mm256_dpbusd_epi32(pair,h,ah);
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    let dot=_mm256_sub_epi32(pair,_mm256_set1_epi32(8*x.sums[g]));
                    sum=_mm256_add_epi32(sum,_mm256_mullo_epi32(dot,scale));
                }
                acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
            }
            _mm256_storeu_ps(out.as_mut_ptr().add(base*8),acc);
        }
    }
}
#[cfg(target_arch="x86_64")]
#[target_feature(enable="avx2,avxvnni")]
unsafe fn tile_avxvnni(tile:&[u8],a:&[Act],blocks:usize,out:&mut[f32],count:usize){
    unsafe{
        let mask=_mm256_set1_epi8(15);
        let mut base=0;
        while base+2<=count{
            let mut acc=[_mm256_setzero_ps();2];
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=[&a[base*blocks+b],&a[(base+1)*blocks+b]];
                let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=[_mm256_setzero_si256();2];
                for g in 0..8{
                    let mut pair=[_mm256_setzero_si256();2];
                    for k in 0..4{
                        let v=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(v,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(v),mask);
                        for j in 0..2{
                            let al=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].low.as_ptr().add(g*16+k*4).cast::<i32>()));
                            let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x[j].high.as_ptr().add(g*16+k*4).cast::<i32>()));
                            pair[j]=_mm256_dpbusd_avx_epi32(pair[j],l,al);
                            pair[j]=_mm256_dpbusd_avx_epi32(pair[j],h,ah);
                        }
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    for j in 0..2{
                       let s=_mm256_sub_epi32(pair[j],_mm256_set1_epi32(8*x[j].sums[g]));
                       sum[j]=_mm256_add_epi32(sum[j],_mm256_mullo_epi32(s,scale));
                    }
                }
                for j in 0..2{
                    acc[j]=_mm256_add_ps(acc[j],_mm256_mul_ps(_mm256_cvtepi32_ps(sum[j]),_mm256_mul_ps(dw,_mm256_set1_ps(x[j].d))));
                }
            }
            for j in 0..2{_mm256_storeu_ps(out.as_mut_ptr().add((base+j)*8),acc[j]);}
            base+=2;
        }
        if base<count{
            let mut acc=_mm256_setzero_ps();
            for (b,w) in tile.chunks_exact(TB).enumerate(){
                let x=&a[base*blocks+b];let dw=_mm256_loadu_ps(w.as_ptr().cast());
                let mut sum=_mm256_setzero_si256();
                for g in 0..8{
                    let mut pair=_mm256_setzero_si256();
                    for k in 0..4{
                        let p=_mm256_loadu_si256(w.as_ptr().add(96+(g*4+k)*32).cast());
                        let l=_mm256_and_si256(p,mask);
                        let h=_mm256_and_si256(_mm256_srli_epi16::<4>(p),mask);
                        let al=_mm256_set1_epi32(std::ptr::read_unaligned(x.low.as_ptr().add(g*16+k*4).cast::<i32>()));
                        let ah=_mm256_set1_epi32(std::ptr::read_unaligned(x.high.as_ptr().add(g*16+k*4).cast::<i32>()));
                        pair=_mm256_dpbusd_avx_epi32(pair,l,al);
                        pair=_mm256_dpbusd_avx_epi32(pair,h,ah);
                    }
                    let scale=_mm256_cvtepu8_epi32(_mm_loadl_epi64(w.as_ptr().add(32+g*8).cast()));
                    let dot=_mm256_sub_epi32(pair,_mm256_set1_epi32(8*x.sums[g]));
                    sum=_mm256_add_epi32(sum,_mm256_mullo_epi32(dot,scale));
                }
                acc=_mm256_add_ps(acc,_mm256_mul_ps(_mm256_cvtepi32_ps(sum),_mm256_mul_ps(dw,_mm256_set1_ps(x.d))));
            }
            _mm256_storeu_ps(out.as_mut_ptr().add(base*8),acc);
        }
    }
}
#[cfg(test)]
#[test]fn test_tile(){
    let raw=(0..512*8).flat_map(|i|(((i as f32*0.731).sin().to_bits()>>16)as u16).to_le_bytes()).collect::<Vec<_>>();
    let row=quantize_matrix(&raw);let tile=tile8(&row,512,8);
    let a=(0..10).map(|j|act(&(0..256).map(|i|((i+j*37)as f32*0.123).cos()).collect::<Vec<_>>())).collect::<Vec<_>>();
    let want=a.chunks_exact(2).flat_map(|a|row.chunks_exact(280).map(move|w|dot_scalar(w,a))).collect::<Vec<_>>();
    #[cfg(target_arch="x86_64")]
    if std::is_x86_feature_detected!("avx2"){let mut got=[0.;40];unsafe{tile_avx(&tile,&a,2,&mut got,5)};for(a,b)in got.iter().zip(&want){assert!((a-b).abs()<1e-4,"{a} {b}");}
       if std::is_x86_feature_detected!("fma"){unsafe{tile_fma(&tile,&a,2,&mut got,5)};for(a,b)in got.iter().zip(&want){assert!((a-b).abs()<1e-4);}}
       if std::is_x86_feature_detected!("avx512vnni")&&std::is_x86_feature_detected!("avx512vl"){unsafe{tile_vnni(&tile,&a,2,&mut got,5)};for(a,b)in got.iter().zip(&want){assert!((a-b).abs()<1e-4);}}
       if std::is_x86_feature_detected!("avxvnni"){unsafe{tile_avxvnni(&tile,&a,2,&mut got,5)};for(a,b)in got.iter().zip(&want){assert!((a-b).abs()<1e-4);}}
    }
}
