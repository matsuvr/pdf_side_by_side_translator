use std::collections::HashMap;
#[derive(Debug)]
pub struct Tensor {pub dims:Vec<usize>,pub ty:u32,pub offset:usize,pub len:usize}
#[derive(Debug)]
pub enum Value {Strings(Vec<String>),Floats(Vec<f32>),Ints(Vec<u32>)}
pub struct Weights {pub(crate) bytes:Vec<u8>,pub meta:HashMap<String,Value>,pub tensors:HashMap<String,Tensor>}
impl Weights {
    pub fn data(&self,t:&Tensor)->&[u8]{&self.bytes[t.offset..t.offset+t.len]}
}
