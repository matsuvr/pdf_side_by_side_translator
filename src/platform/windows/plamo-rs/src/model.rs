//! Fixed P4X86 v7 layout. Runtime input is an owned snapshot supplied by C++.
//! Conversion from the pinned BF16 source is a separate developer-only tool.
use crate::{Result,err,weights::{Weights,Tensor,Value},native,s4};
use rayon::prelude::*;
use serde_json::{Value as J,json};
use std::{collections::HashMap,fs::{File,OpenOptions},io::{BufWriter,Write,Read,Seek,SeekFrom},path::Path};

pub const FILE_BYTES:usize=516_613_376;
pub const TILE:u32=102;
const S4T:u32=107;
const MAGIC:&[u8;8]=b"PLAMX4\x07\0";
fn align64(x:usize)->usize{(x+63)&!63}
struct Spec{
    name:String, original:String, dims:Vec<usize>, ty:u32, norm_offset:Option<f32>
}
fn expected()->Vec<Spec>{
    let make=|name:&str,original:&str,dims:Vec<usize>,ty:u32,norm_offset|Spec{name:name.into(),original:original.into(),dims,ty,norm_offset};
    let mut v=vec![make("token_embd.weight","model.embed_tokens.weight",vec![1536,107520],native::Q4,None),
      make("output.weight","lm_head.weight",vec![1536,107520],TILE,None),
      make("output_norm.weight","model.norm.weight",vec![1536],0,Some(1.))];
    for i in 0..16 {
        for (name,hf,dims,ty,offset) in [
          ("attn_norm.weight","pre_mixer_norm.weight",vec![1536],0,Some(1.)),
          ("attn_q_norm.weight","mixer.q_norm.weight",vec![128],0,Some(1.)),
          ("attn_k_norm.weight","mixer.k_norm.weight",vec![128],0,Some(1.)),
          ("attn_qkv.weight","mixer.qkv_proj.weight",vec![1536,2304],S4T,None),
          ("attn_output.weight","mixer.o_proj.weight",vec![2048,1536],S4T,None),
          ("post_attention_norm","post_mixer_norm.weight",vec![1536],0,Some(0.2)),
          ("ffn_norm.weight","pre_mlp_norm.weight",vec![1536],0,Some(1.)),
          ("ffn_up.weight","mlp.gate_up_proj.weight",vec![1536,12288],S4T,None),
          ("ffn_down.weight","mlp.down_proj.weight",vec![6144,1536],S4T,None),
          ("post_ffw_norm","post_mlp_norm.weight",vec![1536],0,Some(1./5f32.powf(1.5))),
        ]{v.push(make(&format!("blk.{i}.{name}"),&format!("model.layers.layers.{i}.{hf}"),dims,ty,offset));}
    }
    v
}
fn length(dims:&[usize],ty:u32)->usize{
    let elements=dims.iter().product::<usize>();
    if ty==0{elements*4}else if ty==S4T{elements/256*140}else{elements/32*20}
}
pub(crate) fn tile8(src:&[u8],n:usize,m:usize)->Vec<u8>{
    let groups=n/32;let stride=groups*160;let mut buf=vec![0;m*groups*20];
    buf.par_chunks_exact_mut(stride).enumerate().for_each(|(tile,dst)|{
      for g in 0..groups{let b=&mut dst[g*160..(g+1)*160];for r in 0..8{
          let start=((tile*8+r)*groups+g)*20;let s=&src[start..start+20];
          b[r*4..r*4+4].copy_from_slice(&s[..4]);
          for k in 0..4{let p=(k+1)*32+r*4;b[p..p+4].copy_from_slice(&s[4+k*4..8+k*4]);}
      }}
    });buf
}
fn parse_tokenizer(text:&[u8])->Result<HashMap<String,Value>>{
    let text=std::str::from_utf8(text)?;
    let mut tokens=Vec::with_capacity(107520);let mut scores=Vec::with_capacity(107520);let mut types=Vec::with_capacity(107520);
    for line in text.lines(){
        if tokens.len()==107520{return Err(err("vocab size"));}
        let v:J=serde_json::from_str(line)?;
        tokens.push(v[0].as_str().ok_or_else(||err("token"))?.to_owned());
        let score=v[1].as_f64().ok_or_else(||err("score"))? as f32;
        if !score.is_finite(){return Err(err("nonfinite score"));}
        scores.push(score);
        types.push(match v[2].as_str(){Some("NORMAL")=>1,Some("UNKNOWN")=>2,Some("CONTROL")=>3,Some("BYTE")=>6,_=>return Err(err("token type"))});
    }
    if tokens.len()!=107520{return Err(err("vocab size"));}
    Ok(HashMap::from([("tokenizer.ggml.tokens".into(),Value::Strings(tokens)),("tokenizer.ggml.scores".into(),Value::Floats(scores)),("tokenizer.ggml.token_type".into(),Value::Ints(types))]))
}
pub fn open(path:&Path)->Result<Weights>{
    let file=File::open(path)?;
    if file.metadata()?.len()!=FILE_BYTES as u64{return Err(err("unexpected P4X86 file size"));}
    let mut bytes=Vec::new();bytes.try_reserve_exact(FILE_BYTES)?;
    file.take(FILE_BYTES as u64+1).read_to_end(&mut bytes)?;
    from_bytes(bytes)
}
pub fn from_bytes(bytes:Vec<u8>)->Result<Weights>{
    if bytes.len()!=FILE_BYTES || bytes.get(..8)!=Some(MAGIC){return Err(err("expected P4X86 v7"));}
    let len=usize::try_from(u64::from_le_bytes(bytes.get(8..16).ok_or_else(||err("header"))?.try_into()?))?;
    if len>1024*1024{return Err(err("header length"));}
    let header:J=serde_json::from_slice(bytes.get(16..16+len).ok_or_else(||err("truncated header"))?)?;
    if header["model"]!="pfnet/plamo-3-translate-0.8b"||header["format"]!="P4X86"{return Err(err("unexpected model"));}
    let start=align64(16+len);
    let tok=usize::try_from(header["tokenizer_bytes"].as_u64().ok_or_else(||err("tokenizer length"))?)?;
    if tok>16*1024*1024{return Err(err("tokenizer size"));}
    let meta=parse_tokenizer(bytes.get(start..start+tok).ok_or_else(||err("tokenizer bounds"))?)?;
    let specs=expected();
    let entries=header["tensors"].as_array().ok_or_else(||err("tensors"))?;
    if entries.len()!=specs.len(){return Err(err("tensor count"));}
    let mut tensors=HashMap::new();let mut pos=align64(tok);
    for (s,e) in specs.into_iter().zip(entries){
        let len=length(&s.dims,s.ty);
        if e["name"]!=s.name||e["dims"]!=json!(s.dims)||e["type"]!=s.ty||e["offset"]!=pos||e["length"]!=len{return Err(err("unexpected tensor metadata"));}
        let offset=start+pos;
        if bytes.get(offset..offset+len).is_none(){return Err(err("truncated tensor"));}
        tensors.insert(s.name,Tensor{dims:s.dims,ty:s.ty,len,offset});pos=align64(pos+len);
    }
    if start+pos!=bytes.len(){return Err(err("file length mismatch"));}
    Ok(Weights{bytes,meta,tensors})
}
pub fn convert(dir:&Path,out:&Path)->Result<()>{
    let cfg:J=serde_json::from_reader(File::open(dir.join("config.json"))?)?;
    for (k,v) in [("hidden_size",1536),("intermediate_size",6144),("num_hidden_layers",16),("num_attention_heads",16),("num_key_value_heads",1),("head_dim",128),("vocab_size",107520),("window_size",2048),("sliding_window_pattern",8)]{
        if cfg[k].as_u64()!=Some(v){return Err(err(format!("unexpected config {k}")));}
    }
    let mut file=File::open(dir.join("model.safetensors"))?;
    let total=file.metadata()?.len();
    let mut rawlen=[0;8];file.read_exact(&mut rawlen)?;
    let head_len=usize::try_from(u64::from_le_bytes(rawlen))?;
    if head_len>1024*1024{return Err(err("safetensors header too large"));}
    let mut header=vec![0;head_len];file.read_exact(&mut header)?;
    let hf:J=serde_json::from_slice(&header)?;let base=8+head_len as u64;
    let specs=expected();let tok=std::fs::read(dir.join("tokenizer.jsonl"))?;
    if tok.len()>16*1024*1024{return Err(err("tokenizer too large"));}
    parse_tokenizer(&tok)?;
    let mut offset=align64(tok.len());let mut h=vec![];
    for s in &specs{
        let len=length(&s.dims,s.ty);
        h.push(json!({"name":s.name,"dims":s.dims,"type":s.ty,"offset":offset,"length":len}));offset=align64(offset+len);
    }
    let head=serde_json::to_vec(&json!({"model":"pfnet/plamo-3-translate-0.8b","format":"P4X86","tokenizer_bytes":tok.len(),"tensors":h}))?;
    if align64(16+head.len())+offset!=FILE_BYTES{return Err(err("unexpected v7 file size"));}
    let mut f=BufWriter::new(OpenOptions::new().create_new(true).write(true).open(out)?);
    f.write_all(MAGIC)?;f.write_all(&(head.len() as u64).to_le_bytes())?;f.write_all(&head)?;
    f.write_all(&vec![0;align64(16+head.len())-16-head.len()])?;
    f.write_all(&tok)?;f.write_all(&vec![0;align64(tok.len())-tok.len()])?;
    for s in specs{
        let item=&hf[&s.original];
        if item["dtype"]!="BF16"||item["shape"]!=json!(s.dims.iter().rev().copied().collect::<Vec<_>>()){return Err(err("unexpected original tensor"));}
        let offs=item["data_offsets"].as_array().ok_or_else(||err("tensor offsets"))?;
        if offs.len()!=2{return Err(err("tensor offset count"));}
        let a=base.checked_add(offs[0].as_u64().ok_or_else(||err("tensor start"))?).ok_or_else(||err("tensor overflow"))?;
        let b=base.checked_add(offs[1].as_u64().ok_or_else(||err("tensor end"))?).ok_or_else(||err("tensor overflow"))?;
        let want=s.dims.iter().product::<usize>()*2;
        if b>total||b.checked_sub(a)!=Some(want as u64){return Err(err("original tensor bounds"));}
        let mut raw=vec![0;want];file.seek(SeekFrom::Start(a))?;file.read_exact(&mut raw)?;
        let output=if let Some(o)=s.norm_offset{
            raw.chunks_exact(2).flat_map(|x|(native::bf16(x)+o).to_le_bytes()).collect::<Vec<_>>()
        }else if s.ty==S4T{
            s4::tile8(&s4::quantize_matrix(&raw),s.dims[0],s.dims[1])
        }else{
            let q=raw.par_chunks_exact(64).flat_map_iter(native::quantize).collect::<Vec<_>>();
            if s.ty==TILE{tile8(&q,s.dims[0],s.dims[1])}else{q}
        };
        let len=length(&s.dims,s.ty);
        if output.len()!=len{return Err(err("quantized tensor size"));}
        f.write_all(&output)?;f.write_all(&vec![0;align64(len)-len])?;
    }
    f.flush()?;f.get_ref().sync_all()?;Ok(())
}
#[cfg(test)]
mod tests{
    use super::*;
    #[test] fn rejects_truncated(){assert!(from_bytes(vec![0;64]).is_err());}
    #[test] fn output_head_tile_matches_scalar_rows(){
        let n=64;let m=16;let mut src=vec![0;n*m/32*20];
        for(i,b)in src.chunks_exact_mut(20).enumerate(){
            b[..4].copy_from_slice(&(0.001+(i%13)as f32*0.0001).to_le_bytes());
            for(j,q)in b[4..].iter_mut().enumerate(){*q=(i*19+j*53)as u8;}
        }
        let tile=tile8(&src,n,m);let t=Tensor{dims:vec![n,m],ty:TILE,offset:0,len:tile.len()};
        let g=Weights{bytes:tile,meta:HashMap::new(),tensors:HashMap::new()};
        let x=(0..n*5).map(|i|((i*13+3)as f32).sin()).collect::<Vec<_>>();
        let mut got=vec![0.;m*5];native::multiply_tile8(&g,&t,&x,&mut got,5);
        for j in 0..5{for i in 0..m{
            let w=native::row(&g,&t,i).unwrap();
            let want=w.iter().zip(&x[j*n..(j+1)*n]).map(|(a,b)|a*b).sum::<f32>();
            assert!((got[j*m+i]-want).abs()<0.005);
        }}
    }
}
